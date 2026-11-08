"""Chromium shutdown succeeds gracefully or falls back to kill and reap."""
import subprocess
import sys
import unittest
from pathlib import Path
from unittest.mock import Mock, call

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from web_archive_browser_test import close_browser


class BrowserCleanupTest(unittest.TestCase):
    def test_orderly_exit(self):
        process, cdp = Mock(), Mock()
        close_browser(process, cdp)
        cdp.send.assert_called_once_with("Browser.close")
        process.wait.assert_called_once_with(timeout=5)
        process.kill.assert_not_called()

    def test_closed_pipe_still_reaps(self):
        process, cdp = Mock(), Mock()
        cdp.send.side_effect = BrokenPipeError()
        close_browser(process, cdp)
        process.wait.assert_called_once_with(timeout=5)
        process.kill.assert_not_called()

    def test_unresponsive_browser_is_killed_and_reaped(self):
        process, cdp = Mock(), Mock()
        cdp.send.side_effect = RuntimeError("Browser.close timed out")
        process.wait.side_effect = [subprocess.TimeoutExpired("chromium", 5), 0]
        close_browser(process, cdp)
        self.assertEqual(process.mock_calls, [
            call.wait(timeout=5), call.kill(), call.wait(timeout=5)])

    def test_reap_failure_is_reported(self):
        process, cdp = Mock(), Mock()
        process.wait.side_effect = subprocess.TimeoutExpired("chromium", 5)
        with self.assertRaises(subprocess.TimeoutExpired):
            close_browser(process, cdp)
        process.kill.assert_called_once_with()
        self.assertEqual(process.wait.call_count, 2)


if __name__ == "__main__":
    unittest.main()
