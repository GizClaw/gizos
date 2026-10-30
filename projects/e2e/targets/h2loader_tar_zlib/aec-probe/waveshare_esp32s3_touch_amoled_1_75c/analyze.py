"""Validate the single-board log and compare measured far-end echo/headroom.

This deliberately cannot qualify near-end preservation or double-talk.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import re

BOARD = "waveshare_esp32s3_touch_amoled_1_75c"
SPEAKER = (0, 75, 100, 0, 0, 75, 0, 0, 75, 0)
MIC = (63, 63, 63, 63, 47, 47, 47, 79, 79, 79)
BACKGROUND = {63: (0, 3), 47: (4, 6), 79: (7, 9)}


def require(condition: bool, reason: str) -> None:
    if not condition:
        raise ValueError(reason)


def analyze(data: bytes, uid: str, version: str) -> dict:
    text = data.decode("utf-8", errors="replace")
    boots = re.findall(
        rf"H2_AEC_PROBE_BOOT board={BOARD} uid=([0-9a-f]{{12}}) "
        r"version=(\S+) run=([0-9a-f]{16}) scope=single-board-far-echo",
        text,
    )
    require(len(boots) == 1, "one fresh diagnostic boot is required")
    actual_uid, actual_version, run = boots[0]
    require((actual_uid, actual_version) == (uid, version), "wrong device/image identity")
    require(
        re.search(r"Task watchdog|Guru Meditation|CORRUPT HEAP|assert failed|SETUP_FAIL", text)
        is None,
        "the current boot contains a runtime failure",
    )
    profiles = re.findall(
        r"H2_AEC_PROBE_PROFILE run=(\w+) profile=(\d+) microphones=(\d+) "
        r"ref_gain_db=(\d+) nlp=normal rc=(-?\d+)",
        text,
    )
    require(
        profiles
        == [(run, str(p), str(p // 2 + 1), str(p % 2 * 12), "0") for p in range(4)],
        "all four real AEC profiles must initialize in order",
    )
    points = [json.loads(row) for row in re.findall(r"H2_AEC_PROBE_POINT (\{[^\r\n]+\})", text)]
    require(
        [(row["profile"], row["index"]) for row in points]
        == [(p, i) for p in range(4) for i in range(10)],
        "missing, duplicated or unordered measurement windows",
    )
    require(
        re.findall(
            r"H2_AEC_PROBE_DONE run=(\w+) windows=(\d+) rc=(-?\d+) "
            r"confirm=(-?\d+) acoustic_qualified=(\d)", text
        ) == [(run, "40", "0", "0", "0")],
        "complete diagnostic and successful cleanup/managed confirmation are required",
    )
    for row in points:
        p, i = row["profile"], row["index"]
        require(row["run"] == run and row["rc"] == row["cleanup"] == 0, "mixed or failed window")
        require((row["speaker"], row["mic"]) == (SPEAKER[i], MIC[i]), "wrong requested controls")
        require(row["actual_speaker"] == SPEAKER[i], "speaker readback differs")
        require(type(row["actual_mic"]) is int and 0 <= row["actual_mic"] <= 100, "invalid mic readback")
        require(
            (row["mic_lane"], row["ref_lane"], row["mic_mask"])
            == (1, 0, 2 if p < 2 else 10), "wrong ADC/AEC lane mapping",
        )
        far = SPEAKER[i] != 0
        warmup, measured = (96, 96) if far else (32, 64)
        require((row["warmup"], row["measured"]) == (warmup, measured), "wrong observation duration")
        require(
            row["frames"] == row["playback_frames"] == measured
            and row["active_frames"] == (measured if far else 0),
            "missing raw/AEC/DAC frames or silent far-end playback",
        )
        nominal = (warmup + measured) * 32000
        require(nominal - 1000 <= row["elapsed_us"] <= nominal * 5 // 4 + 100000, "invalid real-time cadence")
        for key in ("peak", "energy", "clipped"):
            require(len(row[key]) == 4 and all(type(v) is int and v >= 0 for v in row[key]), "invalid ADC statistics")
        require(all(v <= 32768 for v in row["peak"]), "invalid ADC peak")
    candidates = []
    for p in range(4):
        rows = points[p * 10 : (p + 1) * 10]
        for i in (1, 2, 5, 8):
            far = rows[i]
            quiet = [rows[j] for j in BACKGROUND[MIC[i]]]
            require(all(q["actual_mic"] == far["actual_mic"] for q in quiet), "gain changed between paired noise/far windows")
            power = lambda row, key: row[key] / (row["frames"] * 512)
            raw_power = lambda row: row["energy"][1] / (row["frames"] * 512)
            noise = max(raw_power(q) for q in quiet)
            output_noise = max(power(q, "output_energy") for q in quiet)
            signal = max(0.0, raw_power(far) - noise)
            residual = max(0.0, power(far, "output_energy") - output_noise)
            reference = power(far, "reference_energy")
            echo_db = 10 * math.log10(signal / max(1.0, residual)) if signal > 0 else None
            headroom = all(
                all(row["clipped"][lane] == 0 and row["peak"][lane] < 30000
                    for lane in range(4) if row["mic_mask"] & (1 << lane) or lane == 0)
                and all(row[key] == 0 for key in ("reference_clipped", "output_clipped", "playback_clipped"))
                and all(row[key] < 30000 for key in ("reference_peak", "output_peak", "playback_peak"))
                for row in (*quiet, far)
            )
            stable_noise = max(raw_power(q) for q in quiet) <= max(1.0, min(raw_power(q) for q in quiet)) * 4
            eligible = (headroom and stable_noise and signal >= max(1.0, noise * 10)
                        and reference >= 1000 and echo_db is not None and echo_db >= 10)
            candidates.append(dict(
                profile=p, microphones=p // 2 + 1, reference_gain_db=p % 2 * 12,
                speaker=far["actual_speaker"], mic=far["actual_mic"],
                noise_power=noise, raw_echo_power=signal, reference_power=reference,
                output_residual_power=residual, far_echo_reduction_db=echo_db,
                raw_peak=far["peak"], output_peak=far["output_peak"],
                headroom=headroom, background_stable=stable_noise,
                far_echo_probe_passed=eligible,
            ))
    return dict(
        uid=uid, version=version, run=run, log_sha256=hashlib.sha256(data).hexdigest(),
        complete=True, scope="single-board-far-echo-and-headroom", acoustic_qualified=False,
        near_preservation_assessed=False, double_talk_assessed=False,
        probe_frequencies_hz=[500, 1000, 2500], probe_peak=4478,
        candidates=candidates, points=points,
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--uid", required=True)
    parser.add_argument("--version", required=True)
    args = parser.parse_args()
    try:
        print(json.dumps(analyze(args.log.read_bytes(), args.uid, args.version), indent=2))
    except (ValueError, KeyError, TypeError, OSError) as error:
        parser.exit(2, str(error) + "\n")


if __name__ == "__main__":
    main()
