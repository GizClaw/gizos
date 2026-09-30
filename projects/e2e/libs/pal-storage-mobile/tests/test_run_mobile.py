"""Storage qualification must reject stale, single-process or partial results."""
import copy
import json
from pathlib import Path
import re
import unittest

from run_mobile import verify


CONTRACT = json.loads(Path("projects/e2e/libs/pal-storage-mobile/mobile_e2e.json").read_text())
REGISTRY = Path(CONTRACT["registry"])


class StorageOracleTest(unittest.TestCase):
    def setUp(self):
        entries = re.findall(CONTRACT["registry_pattern"], REGISTRY.read_text())
        self.phases = []
        for phase in (1, 2):
            cases = [{"id": name, "phase": phase, "nonce": 42, "status": "PASS", "rc": 0}
                     for name, selected in entries if int(selected) == phase]
            self.phases.append(dict(contract=1, phase=phase, nonce=42, pid=100 + phase,
                                    passed=len(cases), failed=0, blocked=0, cleanup=0,
                                    teardown=0, rc=0, cases=cases))

    def test_two_process_report_qualifies(self):
        report = verify(self.phases, REGISTRY, "ios", 42, CONTRACT)
        self.assertEqual(report["passed"], 30)
        self.assertEqual(len(report["cases"]), 30)

    def test_same_process_stale_nonce_or_failed_cleanup_cannot_qualify(self):
        for key, value in (("pid", 101), ("nonce", 41), ("phase", 1),
                           ("cleanup", 1), ("teardown", 1), ("blocked", 1)):
            with self.subTest(key=key):
                phases = copy.deepcopy(self.phases)
                phases[1][key] = value
                with self.assertRaises(AssertionError):
                    verify(phases, REGISTRY, "android", 42, CONTRACT)

    def test_partial_duplicate_or_stale_case_cannot_qualify(self):
        for mutation in (lambda cases: cases.reverse(),
                         lambda cases: cases.pop(),
                         lambda cases: cases.__setitem__(0, cases[1]),
                         lambda cases: cases[0].update(nonce=41),
                         lambda cases: cases[0].update(status="NOT_RUN"),
                         lambda cases: cases[0].update(rc=-1)):
            phases = copy.deepcopy(self.phases)
            mutation(phases[1]["cases"])
            with self.assertRaises(AssertionError):
                verify(phases, REGISTRY, "android", 42, CONTRACT)


if __name__ == "__main__":
    unittest.main()
