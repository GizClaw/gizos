"""A bounded TLS dispatch must not enter an opaque legacy callback."""

import subprocess
import sys
import unittest

from python.runfiles import runfiles


class NetTLSDeadline(unittest.TestCase):
    def check_bounded_rejection(self, mode):
        process = subprocess.Popen(
            [PROBE, mode],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        try:
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                # Release the callback before reporting failure, so even the
                # pre-fix path has a complete subprocess cleanup owner.
                stdout, stderr = process.communicate(input=b"release\n", timeout=2)
                self.fail(
                    "bounded TLS entered the stalled legacy callback despite "
                    "its 25 ms budget:\n" + (stdout + stderr).decode(errors="replace")
                )
            stdout, stderr = process.communicate()
            self.assertEqual(process.returncode, 0, (stdout + stderr).decode(errors="replace"))
            self.assertNotIn(b"LEGACY_ENTERED", stdout)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()

    def test_missing_capability_does_not_enter_stalled_legacy(self):
        self.check_bounded_rejection("missing")

    def test_explicit_rejection_does_not_enter_stalled_legacy(self):
        self.check_bounded_rejection("rejected")

    def test_ordinary_legacy_entry_is_preserved(self):
        result = subprocess.run(
            [PROBE, "ordinary"],
            input=b"release\n",
            capture_output=True,
            timeout=2,
        )
        self.assertEqual(result.returncode, 0, (result.stdout + result.stderr).decode(errors="replace"))
        self.assertIn(b"LEGACY_ENTERED", result.stdout)


if __name__ == "__main__":
    PROBE = runfiles.Create().Rlocation(sys.argv[1])
    if not PROBE:
        raise RuntimeError("PAL Net TLS probe is missing from runfiles")
    unittest.main(argv=[sys.argv[0]])
