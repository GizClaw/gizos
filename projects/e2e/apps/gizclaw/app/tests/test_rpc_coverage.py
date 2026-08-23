from pathlib import Path
import subprocess
import unittest


class RpcCoverageTest(unittest.TestCase):
    def test_public_surface_has_e2e_evidence(self) -> None:
        checker = Path(__file__).with_name("check_rpc_coverage.sh")
        result = subprocess.run(
            [checker],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertRegex(
            result.stdout,
            r"^H2_GIZCLAW_E2E coverage=PASS methods=\d+ wrappers=\d+ "
            r"evidence=\d+\n$",
        )


if __name__ == "__main__":
    unittest.main()
