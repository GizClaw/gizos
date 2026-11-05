"""Report rejection tests; generated metadata is never device evidence."""

import json
import unittest

from analyze import BOARD, MIC, SPEAKER, analyze

UID = "0123456789ab"
VERSION = "test-only"
RUN = "0123456789abcdef"


def log(change=None):
    lines = [f"H2_AEC_PROBE_BOOT board={BOARD} uid={UID} version={VERSION} run={RUN} scope=single-board-far-echo"]
    for p in range(4):
        lines.append(f"H2_AEC_PROBE_PROFILE run={RUN} profile={p} microphones={p // 2 + 1} ref_gain_db={p % 2 * 12} nlp=normal rc=0")
        for i in range(10):
            far = SPEAKER[i] != 0
            warmup, frames = (96, 96) if far else (32, 64)
            n = frames * 512
            row = dict(
                run=RUN, profile=p, index=i, speaker=SPEAKER[i], mic=MIC[i],
                actual_speaker=SPEAKER[i], actual_mic=MIC[i],
                frames=frames, playback_frames=frames, active_frames=frames if far else 0,
                elapsed_us=(warmup + frames) * 32000, warmup=warmup, measured=frames,
                mic_lane=1, ref_lane=0, mic_mask=2 if p < 2 else 10,
                peak=[1000] * 4, energy=[n * (10000 if far else 100)] * 4,
                clipped=[0] * 4, reference_peak=1000,
                reference_energy=n * (2000 if far else 100), reference_clipped=0,
                output_peak=1000, output_energy=n * (200 if far else 100), output_clipped=0,
                playback_peak=4478 if far else 0, playback_clipped=0, rc=0, cleanup=0,
            )
            if change is not None:
                change(row)
            lines.append("H2_AEC_PROBE_POINT " + json.dumps(row))
    lines.append(f"H2_AEC_PROBE_DONE run={RUN} windows=40 rc=0 confirm=0 acoustic_qualified=0")
    return ("\n".join(lines) + "\n").encode()


class ReportTests(unittest.TestCase):
    def test_far_echo_never_grants_full_acoustic_qualification(self):
        report = analyze(log(), UID, VERSION)
        self.assertTrue(all(row["far_echo_probe_passed"] for row in report["candidates"]))
        self.assertFalse(report["acoustic_qualified"])
        self.assertFalse(report["near_preservation_assessed"])
        self.assertFalse(report["double_talk_assessed"])

    def test_silent_microphone_cannot_pass(self):
        report = analyze(log(lambda row: row.update(energy=[0] * 4)), UID, VERSION)
        self.assertFalse(any(row["far_echo_probe_passed"] for row in report["candidates"]))

    def test_adc_headroom_violation_cannot_pass(self):
        report = analyze(log(lambda row: row.update(peak=[32767] * 4, clipped=[1] * 4)), UID, VERSION)
        self.assertFalse(any(row["far_echo_probe_passed"] for row in report["candidates"]))

    def test_silent_dac_far_window_is_invalid(self):
        with self.assertRaisesRegex(ValueError, "silent far-end"):
            analyze(log(lambda row: row.update(active_frames=0)), UID, VERSION)

    def test_missing_or_duplicate_window_is_invalid(self):
        lines = log().splitlines(keepends=True)
        with self.assertRaisesRegex(ValueError, "measurement windows"):
            analyze(b"".join(lines[:3] + lines[4:]), UID, VERSION)
        with self.assertRaisesRegex(ValueError, "measurement windows"):
            analyze(b"".join(lines[:3] + [lines[2]] + lines[3:]), UID, VERSION)

    def test_runtime_failure_rejects_terminal_success(self):
        with self.assertRaisesRegex(ValueError, "runtime failure"):
            analyze(log() + b"Task watchdog got triggered\n", UID, VERSION)

    def test_wrong_identity_and_reference_mapping(self):
        with self.assertRaisesRegex(ValueError, "identity"):
            analyze(log(), "ffffffffffff", VERSION)
        with self.assertRaisesRegex(ValueError, "lane mapping"):
            analyze(log(lambda row: row.update(ref_lane=2)), UID, VERSION)


if __name__ == "__main__":
    unittest.main()
