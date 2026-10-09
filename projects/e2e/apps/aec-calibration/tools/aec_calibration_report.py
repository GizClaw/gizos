"""Validate one public acoustic probe run; PCM and device policy stay elsewhere."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys


class InvalidCalibration(ValueError):
    pass


def integer(record, key, minimum=0, maximum=None):
    value = record.get(key)
    if type(value) is not int or value < minimum or (maximum is not None and value > maximum):
        raise InvalidCalibration(f"invalid {key}")
    return value


def boolean(record, key):
    value = record.get(key)
    if type(value) is not bool:
        raise InvalidCalibration(f"invalid {key}")
    return value


def array(record, key, count, maximum=None):
    values = record.get(key)
    if not isinstance(values, list) or len(values) != count:
        raise InvalidCalibration(f"invalid {key}")
    for value in values:
        integer({key: value}, key, maximum=maximum)
    return values


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise InvalidCalibration("duplicate JSON field")
        result[key] = value
    return result


def _limits(begin):
    if begin.get("schema") != 1 or begin.get("probe") != "three-band-two-level-v2":
        raise InvalidCalibration("unsupported calibration schema/probe")
    count = integer(begin, "count", 1, 16)
    integer(begin, "selection", 0, 2)
    rate = integer(begin, "sample_rate", 0, 48000)
    samples = integer(begin, "frame_samples", 0, 1024)
    bins = array(begin, "far_bins", 3) + array(begin, "near_bins", 3)
    if rate == 0:
        if samples != 0 or any(bins):
            raise InvalidCalibration("inconsistent unavailable Audio format")
    else:
        expected = [(frequency * samples + rate // 2) // rate
                    for frequency in (437, 1031, 2156, 719, 1438, 2938)]
        if rate < 8000 or samples < 256 or bins != expected or len(set(bins)) != 6 or min(bins) < 1 or max(bins) >= samples // 2:
            raise InvalidCalibration("wrong probe frequency bins")
    warmup = integer(begin, "warmup", 4, 4096)
    measured = integer(begin, "measurement", 8, 4096)
    integer(begin, "stability", measured, 4096)
    integer(begin, "io_timeout_ms", 1, 10000)
    integer(begin, "source_timeout_ms", 1, 30000)
    integer(begin, "max_cadence_milli", 1000, 2000)
    integer(begin, "cadence_margin_ms", 0, 1000)
    integer(begin, "max_residual_milli", 1, 999)
    integer(begin, "min_near_milli", 1, 1000)
    integer(begin, "min_double_milli", 1, 1000)
    integer(begin, "min_reference_power", 1)
    integer(begin, "min_snr", 2, 1000)
    peak = integer(begin, "peak_limit", 1, 32759)
    amplitude = array(begin, "amplitude", 2)
    if not 0 < amplitude[0] < amplitude[1] or amplitude[1] * 3 >= peak:
        raise InvalidCalibration("unsafe or unordered probe amplitudes")
    return count, warmup


def _measurement(record, begin, phase):
    frames = integer(record, "frames", maximum=4096)
    playback = integer(record, "playback_frames", maximum=4096)
    active = integer(record, "playback_active_frames", maximum=playback)
    samples = integer(record, "samples", maximum=4096 * 1024)
    channels = integer(record, "raw_channels", maximum=8)
    mask = integer(record, "mic_mask", maximum=255)
    microphone = integer(record, "mic_lane", maximum=7)
    reference = integer(record, "reference_lane", maximum=7)
    if frames and (channels < 2 or not mask or mask >= 1 << channels or
                   reference >= channels or mask & (1 << reference) or
                   microphone >= channels or not mask & (1 << microphone)):
        raise InvalidCalibration("invalid ADC microphone/reference lanes")
    if samples != frames * begin["frame_samples"]:
        raise InvalidCalibration("inconsistent complete-frame sample count")
    for key in ("mic_energy", "reference_energy", "aec_reference_energy", "output_energy"):
        integer(record, key, maximum=samples * 32768**2)
    for key in ("near_mic", "near_output", "far_mic", "far_output"):
        array(record, key, 3, frames * 2 * 32768**2)
    peaks = array(record, "peak", 4, 32768)
    clipped = integer(record, "clipped", maximum=samples * 10)
    playback_peak = integer(record, "playback_peak", maximum=32768)
    playback_clipped = integer(record, "playback_clipped", maximum=playback * begin["frame_samples"])
    if (max(peaks) >= begin["peak_limit"] and clipped == 0) or (
            playback_peak >= begin["peak_limit"] and playback_clipped == 0):
        raise InvalidCalibration("peak contradicts clipping/headroom count")
    integer(record, "elapsed_ms")
    source_ms = integer(record, "source_control_ms")
    source_max = integer(record, "source_control_max_ms")
    source_calls = integer(record, "source_control_calls", maximum=2**32 - 1)
    if source_max > source_ms or source_ms > source_calls * source_max:
        raise InvalidCalibration("inconsistent external source-control timing")
    integer(record, "diagnostic_rc", -(2**31), 2**31 - 1)
    if begin["sample_rate"] == 0:
        if frames or playback or samples or any(peaks):
            raise InvalidCalibration("measurements without an available Audio format")
        return False
    expected = begin["stability"] if phase == 4 else begin["measurement"]
    nominal = (expected + begin["warmup"]) * begin["frame_samples"] * 1000 // begin["sample_rate"]
    minimum = max(0, nominal - 1)
    maximum = nominal * begin["max_cadence_milli"] // 1000 + begin["cadence_margin_ms"]
    return (frames == expected and playback == expected and
            active == (0 if phase in (0, 2) else expected) and
            minimum <= record["elapsed_ms"] <= maximum and
            source_calls == (1 if phase in (0, 1) else 2) and
            source_max <= begin["source_timeout_ms"] and
            record["diagnostic_rc"] == 0 and clipped == 0 and playback_clipped == 0)


def _accept(phases, begin):
    for level in range(2):
        measurements = [phases[(level, phase)] for phase in range(5)]
        if not all(_measurement(m, begin, phase) for phase, m in enumerate(measurements)):
            return False
        noise, far, near, double, stable = measurements
        power = lambda m, key: m[key] // m["samples"]
        snr = begin["min_snr"]
        background = power(noise, "mic_energy")
        echo = power(far, "mic_energy")
        residual = max(0, power(far, "output_energy") - power(noise, "output_energy"))
        if (echo <= (background + 1) * snr or
                power(far, "reference_energy") < begin["min_reference_power"] or
                power(far, "aec_reference_energy") < begin["min_reference_power"] or
                far["playback_peak"] == 0 or
                residual * 1000 > max(0, echo - background) * begin["max_residual_milli"] or
                power(near, "reference_energy") > power(far, "reference_energy") // snr):
            return False
        band = lambda m, key, b: m[key][b] // m["frames"]
        for b in range(3):
            far_floor = band(noise, "far_mic", b)
            far_raw = max(0, band(far, "far_mic", b) - far_floor)
            far_out = max(0, band(far, "far_output", b) - band(noise, "far_output", b))
            if (far_raw <= (far_floor + 1) * snr or
                    far_out * 1000 > far_raw * begin["max_residual_milli"]):
                return False
            floor = band(noise, "near_mic", b)
            raw = max(0, band(near, "near_mic", b) - floor)
            output = max(0, band(near, "near_output", b) - band(noise, "near_output", b))
            if (raw <= (floor + 1) * snr or
                    output * 1000 < raw * begin["min_near_milli"]):
                return False
            for dt in (double, stable):
                dt_raw = max(0, band(dt, "near_mic", b) - band(far, "near_mic", b))
                dt_out = max(0, band(dt, "near_output", b) - band(far, "near_output", b))
                dt_far_raw = max(0, band(dt, "far_mic", b) - band(near, "far_mic", b))
                dt_far_out = max(0, band(dt, "far_output", b) - band(near, "far_output", b))
                if (dt["playback_peak"] == 0 or
                        power(dt, "reference_energy") < begin["min_reference_power"] or
                        power(dt, "aec_reference_energy") < begin["min_reference_power"] or
                        not far_raw // 2 <= dt_far_raw <= far_raw * 2 or
                        dt_far_out * 1000 > dt_far_raw * begin["max_residual_milli"] or
                        not raw // 2 <= dt_raw <= raw * 2 or
                        dt_out * 1000 < dt_raw * begin["min_near_milli"] or
                        dt_out * 1000 < output * begin["min_double_milli"]):
                    return False
    return True


def validate(log: bytes | str, run: str | None = None) -> dict:
    text = log.decode("utf-8", errors="replace") if isinstance(log, bytes) else log
    begin = summary = None
    candidates, phases = {}, {}
    for row in text.splitlines():
        if "AEC_CALIBRATION " not in row:
            continue
        match = re.search(r"AEC_CALIBRATION (\{.*\})$", row)
        if not match:
            raise InvalidCalibration("truncated calibration record")
        try:
            record = json.loads(match[1], object_pairs_hook=unique_object)
        except json.JSONDecodeError as error:
            raise InvalidCalibration("invalid calibration JSON") from error
        if not isinstance(record, dict) or not re.fullmatch(r"[0-9a-f]{16}", str(record.get("run"))):
            raise InvalidCalibration("invalid calibration run")
        if run is not None and record["run"] != run:
            raise InvalidCalibration("calibration belongs to another run")
        kind = record.get("kind")
        if kind == "begin":
            if begin is not None:
                raise InvalidCalibration("multiple calibration runs")
            _limits(record)
            begin = record
            continue
        if begin is None or summary is not None or record["run"] != begin["run"]:
            raise InvalidCalibration("missing begin, late record or mixed runs")
        if kind == "candidate":
            index = integer(record, "index", maximum=begin["count"] - 1)
            if index in candidates:
                raise InvalidCalibration("duplicate candidate")
            requested, actual = array(record, "requested", 2, 100), array(record, "actual", 2, 100)
            if requested[0] == 0:
                raise InvalidCalibration("silent speaker candidate")
            integer(record, "rc", -(2**31), 2**31 - 1)
            boolean(record, "pareto")
            candidates[index] = record
        elif kind == "phase":
            index = integer(record, "index", maximum=begin["count"] - 1)
            level = integer(record, "level", maximum=1)
            phase = integer(record, "phase", maximum=4)
            key = (index, level, phase)
            if index not in candidates or key in phases:
                raise InvalidCalibration("missing candidate or duplicate phase")
            _measurement(record, begin, phase)
            phases[key] = record
        elif kind == "summary":
            summary = record
        else:
            raise InvalidCalibration("unknown calibration record")
    if begin is None or summary is None or len(candidates) != begin["count"]:
        raise InvalidCalibration("missing calibration terminal records")
    complete = boolean(summary, "complete")
    retained = boolean(summary, "retained")
    selected = boolean(summary, "selected")
    execution = integer(summary, "rc", -(2**31), 2**31 - 1)
    integer(summary, "selected_index", maximum=begin["count"] - 1)
    if complete and begin["sample_rate"] == 0:
        raise InvalidCalibration("unavailable Audio cannot complete a measurement search")
    if integer(summary, "count") != begin["count"]:
        raise InvalidCalibration("wrong summary count")
    if len(phases) != begin["count"] * 2 * 5:
        raise InvalidCalibration("missing phase evidence")
    passed = []
    for i, c in candidates.items():
        if c["rc"] == 0:
            if c["actual"][0] == 0 or not _accept(
                    {(level, phase): phases[(i, level, phase)] for level in range(2) for phase in range(5)}, begin):
                raise InvalidCalibration("candidate PASS contradicts acoustic evidence")
            passed.append(i)
    frontier = [i for i in passed if not any(
        candidates[j]["actual"][0] >= candidates[i]["actual"][0] and
        candidates[j]["actual"][1] >= candidates[i]["actual"][1] and
        candidates[j]["actual"] != candidates[i]["actual"] for j in passed)]
    if complete:
        if integer(summary, "passed") != len(passed) or integer(summary, "pareto_count") != len(frontier):
            raise InvalidCalibration("wrong measured/Pareto counts")
        if any(c["pareto"] != (i in frontier) for i, c in candidates.items()):
            raise InvalidCalibration("wrong Pareto membership")
    elif integer(summary, "passed") or integer(summary, "pareto_count") or selected:
        raise InvalidCalibration("incomplete search cannot recommend a candidate")
    cleanup = integer(summary, "cleanup_rc", -(2**31), 2**31 - 1)
    if retained != (cleanup != 0) or (retained and selected):
        raise InvalidCalibration("unsafe cleanup/selection")
    if selected:
        if begin["selection"] == 0 or not complete or not frontier or execution != 0:
            raise InvalidCalibration("selection contradicts policy")
        order = (0, 1) if begin["selection"] == 1 else (1, 0)
        expected = max(frontier, key=lambda i: tuple(candidates[i]["actual"][axis] for axis in order))
        if integer(summary, "selected_index", maximum=begin["count"] - 1) != expected:
            raise InvalidCalibration("selection is not the best measured candidate for its policy")
    elif complete and frontier and begin["selection"] != 0 and not retained and execution == 0:
        raise InvalidCalibration("missing requested selection")
    eligible = complete and not retained and bool(passed) and cleanup == 0
    if execution == 0 and not eligible:
        raise InvalidCalibration("execution result contradicts acoustic/cleanup qualification")
    qualified = eligible and execution == 0
    return {"begin": begin, "candidates": list(candidates.values()),
            "phases": list(phases.values()), "summary": summary,
            "qualified": qualified}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--run")
    args = parser.parse_args()
    try:
        result = validate(args.log.read_bytes(), args.run)
    except (InvalidCalibration, OSError) as error:
        print(str(error), file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2))
    return 0 if result["qualified"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
