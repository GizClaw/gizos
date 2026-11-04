"""Validate real P4 single-board gain measurements; never grant voice acceptance."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import re

BOARD = "waveshare_esp32p4_wifi6_touch_lcd_4_3"
SPEAKERS = (93, 97, 100)
MICROPHONES = (63, 79, 95, 100)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def analyze(data: bytes, uid: str, version: str) -> dict:
    text = data.decode(errors="replace")
    boots = re.findall(
        rf"H2_MFG_GAIN_BOOT board={BOARD} uid=(\w+) version=(\S+) run=(\w+)"
        r" codec_max=bf ref_gain=16000 mic_mask=10 ref_lane=0"
        r" probe_peak=4478,22390 scope=single-board-far-echo",
        text,
    )
    require(len(boots) == 1, "one fresh measurement boot is required")
    actual_uid, actual_version, run = boots[0]
    require((actual_uid, actual_version) == (uid, version), "wrong device/image")
    require(re.fullmatch("[0-9a-f]{16}", run) is not None, "invalid run identity")
    require(
        re.search(
            r"Guru Meditation|Task watchdog|assert failed|CORRUPT HEAP|"
            r"H2_MFG_GAIN_(?:SETUP|CLEANUP)_FAIL",
            text,
        )
        is None,
        "runtime or cleanup failure",
    )
    done = re.findall(
        r"H2_MFG_GAIN_DONE run=(\w+) windows=(\d+) rc=(-?\d+)"
        r" cleanup=(-?\d+)"
        r" acoustic_qualified=(\d) controls_persisted=(\d)",
        text,
    )
    require(done == [(run, "48", "0", "0", "0", "0")],
            "measurement and control restoration must complete")
    rows = [
        json.loads(row)
        for row in re.findall(r"H2_MFG_GAIN_POINT (\{[^\r\n]+\})", text)
    ]
    require(
        [(row["candidate"], row["index"]) for row in rows]
        == [(candidate, index) for candidate in range(12) for index in range(4)],
        "missing, duplicated or unordered windows",
    )
    for row in rows:
        candidate, index = row["candidate"], row["index"]
        speaker = 0 if index in (0, 3) else SPEAKERS[candidate // 4]
        microphone = MICROPHONES[candidate % 4]
        require(row["run"] == run, "mixed runs")
        require(row["rc"] == row["cleanup"] == 0, "failed window")
        require(
            (row["speaker"], row["mic"], row["scale"])
            == (speaker, microphone, 5 if index == 2 else 1),
            "wrong requested controls or signal level",
        )
        require(
            (row["actual_speaker"], row["actual_mic"]) == (speaker, microphone),
            "controls were not read back",
        )
        warmup = 32 if speaker == 0 else 96
        require(
            (row["warmup"], row["measured"], row["frames"], row["dac_frames"])
            == (warmup, 96, 96, 96),
            "missing ADC/AEC/DAC frames",
        )
        require(row["active"] == (0 if speaker == 0 else 96),
                "missing or unexpected DAC signal")
        nominal = (warmup + 96) * 32000
        require(nominal - 1000 <= row["elapsed_us"] <= nominal * 5 // 4 + 100000,
                "invalid real-time cadence")
        require(row["dac_peak"] == (0 if speaker == 0 else 4478 * row["scale"]),
                "wrong actual DAC signal amplitude")
        for key in ("peak", "energy", "clipped"):
            require(
                len(row[key]) == 4
                and all(type(value) is int and value >= 0 for value in row[key]),
                "invalid ADC statistics",
            )
        require(all(value <= 32768 for value in row["peak"]), "invalid ADC peak")
    candidates = []
    for candidate in range(12):
        windows = rows[candidate * 4 : (candidate + 1) * 4]
        quiet = (windows[0], windows[3])
        samples = 96 * 512
        raw_power = lambda row: row["energy"][1] / samples
        noise = max(map(raw_power, quiet))
        output_noise = max(row["output_energy"] / samples for row in quiet)
        stable = noise <= max(1.0, min(map(raw_power, quiet))) * 4
        levels = []
        for row in windows[1:3]:
            signal = max(0.0, raw_power(row) - noise)
            residual = max(0.0, row["output_energy"] / samples - output_noise)
            reduction = (
                10 * math.log10(signal / max(1.0, residual)) if signal > 0 else None
            )
            headroom = all(
                all(w["peak"][lane] < 30000 and w["clipped"][lane] == 0
                    for lane in (0, 1, 3))
                and all(w[key] < 30000
                        for key in ("reference_peak", "output_peak", "dac_peak"))
                and all(w[key] == 0 for key in
                        ("reference_clipped", "output_clipped", "dac_clipped"))
                for w in (*quiet, row)
            )
            reasons = []
            if not stable:
                reasons.append("background_unstable")
            if not headroom:
                reasons.append("headroom_exceeded")
            if signal < max(1.0, noise * 10):
                reasons.append("echo_input_too_weak")
            if min(row["reference_energy"], row["energy"][0]) / samples < 1000:
                reasons.append("reference_inactive")
            if reduction is None or reduction < 10:
                reasons.append("echo_reduction_below_10dB")
            levels.append(dict(
                scale=row["scale"], echo_reduction_db=reduction,
                raw_echo_power=signal, residual_power=residual,
                raw_peak=row["peak"], reference_peak=row["reference_peak"],
                output_peak=row["output_peak"], dac_peak=row["dac_peak"],
                headroom=headroom, passed=not reasons, reasons=reasons,
            ))
        candidates.append(dict(
            speaker=SPEAKERS[candidate // 4], mic=MICROPHONES[candidate % 4],
            noise_power=noise, background_stable=stable, levels=levels,
            far_probe_passed=all(level["passed"] for level in levels),
        ))
    eligible = [row for row in candidates if row["far_probe_passed"]]
    selection = max(eligible, key=lambda row: (row["speaker"], row["mic"]),
                    default=None)
    return dict(
        board=BOARD, uid=uid, version=version, run=run,
        log_sha256=hashlib.sha256(data).hexdigest(), complete=True,
        scope="single-board-far-echo-headroom-two-levels",
        acoustic_qualified=False, near_preservation_assessed=False,
        double_talk_assessed=False, analog_distortion_assessed=False,
        controls_persisted=False, candidates=candidates,
        far_probe_speaker_first=selection, points=rows,
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--uid", required=True)
    parser.add_argument("--version", required=True)
    args = parser.parse_args()
    try:
        print(json.dumps(analyze(args.log.read_bytes(), args.uid, args.version),
                         indent=2))
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(2, str(error) + "\n")


if __name__ == "__main__":
    main()
