"""Validate Connectivity case logs using doubles, never as real throughput."""

import os
from pathlib import Path
import subprocess
import unittest

import api_coverage

# Mirrors SPEED_BYTES in app/src/h2_gizclaw_e2e_connectivity.c.
SPEED_BYTES = 2 * 1024 * 1024


class ConnectivityCoverageTest(unittest.TestCase):
    def run_case(self, failure=0, mode=0, budget=0, clock=0):
        root = api_coverage.repository_root()
        executable = root / "projects/e2e/apps/gizclaw/app/gizclaw_e2e_connectivity_test"
        if os.name == "nt":
            executable = Path(str(executable) + ".exe")
        run = subprocess.run([str(executable), "--emit-evidence", str(failure), str(mode),
                              str(budget), str(clock)],
                             capture_output=True, text=True, timeout=30, check=False)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertNotIn("synthetic-registration", run.stdout + run.stderr)
        rules = api_coverage.requirements()
        api_coverage.validate_inventory(rules, (root / "libs/gizclaw/tests/public_api.inc").read_text())
        result = api_coverage.audit(run.stdout.splitlines(keepends=True), rules,
                                    endpoint="example.invalid:9821", backend="h2peer",
                                    profile="default", platform="macos", process_exit_code=run.returncode)
        # A boundary test and partial case can never certify full live E2E.
        self.assertFalse(result["valid"])
        observed = {row["symbol"] for row in result["functions"] if row["status"] == "covered"}
        return run.stdout, observed, result

    def test_twelve_functions_and_all_measurement_rows(self):
        log, observed, result = self.run_case()
        expected = {rule.symbol for rule in api_coverage.requirements() if rule.case == "connectivity"}
        self.assertEqual(len(expected), 12)
        self.assertEqual(observed, expected)
        self.assertEqual(result["missing"], 178)
        hooks = [dict(field.split("=", 1) for field in line.split()[1:])
                 for line in log.splitlines() if " stage=speedtest-hooks " in line]
        self.assertEqual(len(hooks), 6)
        self.assertEqual({row["request"] for row in hooks},
                         {str(identity) for identity in range(11, 17)})
        for row in hooks:
            self.assertEqual(row["result"], "PASS")
            self.assertEqual(row["rc"], "0")
            self.assertEqual(row["bytes"], str(SPEED_BYTES))
            # Both directions move every byte through a data callback: the
            # upload reader and the download writer each see several chunks.
            self.assertGreater(int(row["chunks"]), 1)
        rows = [dict(field.split("=", 1) for field in line.split()[1:])
                for line in log.splitlines() if " stage=speedtest " in line]
        self.assertEqual(len(rows), 12)
        self.assertEqual({(row["api"], row["direction"], row["attempt"]) for row in rows},
                         {(api, direction, str(attempt)) for api in ("req", "rpc")
                          for direction in ("upload", "download") for attempt in (1, 2, 3)})
        for row in rows:
            self.assertEqual(row["bytes"], str(SPEED_BYTES))
            self.assertEqual(row["expected_bytes"], row["bytes"])
            self.assertEqual(row["result"], "PASS")
            self.assertEqual(row["rc"], "0")
            self.assertEqual(row["integrity"], "length-ack-only" if row["direction"] == "upload"
                             else "callback-length-verified")
            self.assertEqual(int(row["bps"]), int(row["bytes"]) * 8000 // int(row["transfer_ms"]))

    def test_failures_cannot_certify_partial_success(self):
        for failure, mode, budget, clock in (
                [(stage, 0, 0, 0) for stage in range(1, 46)] +
                [(0, mode, 0, 0) for mode in range(1, 13)] +
                [(0, 0, budget, 0) for budget in range(1, 19)] +
                [(0, 0, 0, clock) for clock in range(1, 25)]):
            with self.subTest(failure=failure, mode=mode, budget=budget, clock=clock):
                log, observed, result = self.run_case(failure, mode, budget, clock)
                self.assertIn("status=FAIL", log)
                self.assertEqual(observed, set())
                self.assertEqual(result["missing"], 190)

    def test_missing_dispatch_cannot_certify_request_speed(self):
        log, _, _ = self.run_case()
        lines = [line for line in log.splitlines(keepends=True)
                 if "stage=connectivity-chunks " not in line]
        result = api_coverage.audit(lines, api_coverage.requirements(),
                                    endpoint="example.invalid:9821", backend="h2peer",
                                    profile="default", platform="macos", process_exit_code=0)
        observed = {row["symbol"] for row in result["functions"]
                    if row["status"] == "covered"}
        self.assertNotIn("h2_gizclaw_req_create_speedtest", observed)
        self.assertNotIn("h2_gizclaw_resp_parse_speedtest", observed)
        self.assertIn("h2_gizclaw_rpc_speedtest", observed)


if __name__ == "__main__":
    unittest.main()
