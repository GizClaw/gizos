"""Check C-emitted measurements with an independent report verifier."""

import json
import subprocess
import unittest

from python.runfiles import runfiles

from aec_calibration_report import InvalidCalibration, validate


class Reports(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        executable = runfiles.Create().Rlocation(
            "_main/projects/e2e/apps/aec-calibration/app/calibration_test")
        cls.executable = executable
        cls.log = subprocess.check_output([executable, "--report"]).decode()
        cls.records = [json.loads(row.split(" ", 1)[1]) for row in cls.log.splitlines()]

    def altered(self, mutate):
        records = json.loads(json.dumps(self.records))
        mutate(records)
        return "\n".join("AEC_CALIBRATION " + json.dumps(row) for row in records)

    def test_c_report_has_two_pareto_candidates(self):
        result = validate(self.log, "0000000000000001")
        self.assertTrue(result["qualified"])
        self.assertEqual(result["summary"]["passed"], 3)
        self.assertEqual(result["summary"]["pareto_count"], 2)
        self.assertFalse(result["summary"]["selected"])

    def test_unavailable_audio_is_an_honest_unsupported_report(self):
        log = subprocess.check_output([self.executable, "--unsupported-report"])
        result = validate(log)
        self.assertFalse(result["qualified"])
        self.assertEqual(result["summary"]["rc"], -3)
        self.assertFalse(result["summary"]["complete"])

    def test_reject_missing_near_and_double_talk_preservation(self):
        for phase in (2, 3, 4):
            def mute(records):
                for record in records:
                    if record["kind"] == "phase" and record["index"] == 1 and record["phase"] == phase:
                        record["near_output"] = [0, 0, 0]
                        record["output_energy"] = 0
            with self.assertRaises(InvalidCalibration):
                validate(self.altered(mute))

    def test_reject_slow_cadence(self):
        def slow(records):
            next(r for r in records if r["kind"] == "phase" and r["index"] == 1)["elapsed_ms"] = 100000
        with self.assertRaises(InvalidCalibration):
            validate(self.altered(slow))

    def test_reject_late_external_source_control(self):
        def slow_source(records):
            record = next(r for r in records if r["kind"] == "phase" and r["index"] == 1 and r["phase"] == 2)
            record.update(source_control_ms=6000, source_control_max_ms=6000)
        with self.assertRaises(InvalidCalibration):
            validate(self.altered(slow_source))

    def test_reject_clip_or_reference_loss(self):
        for field, value in (("clipped", 1), ("playback_clipped", 1), ("reference_energy", 0),
                             ("aec_reference_energy", 0)):
            def alter(records):
                next(r for r in records if r["kind"] == "phase" and r["index"] == 1 and r["phase"] == 1)[field] = value
            with self.assertRaises(InvalidCalibration):
                validate(self.altered(alter))

    def test_reject_forged_pareto_and_selected_policy(self):
        def forged(records):
            next(r for r in records if r["kind"] == "candidate" and r["index"] == 3)["pareto"] = True
        with self.assertRaises(InvalidCalibration):
            validate(self.altered(forged))
        with self.assertRaises(InvalidCalibration):
            validate(self.altered(lambda records: records[-1].update(selected=True, selected_index=3)))

    def test_reject_incomplete_mixed_or_duplicate(self):
        for log in (self.log.rsplit("\n", 2)[0], self.log + self.log,
                    self.log.replace('"run":"0000000000000001"', '"run":"0000000000000002"', 1)):
            with self.assertRaises(InvalidCalibration):
                validate(log, "0000000000000001")


if __name__ == "__main__":
    unittest.main()
