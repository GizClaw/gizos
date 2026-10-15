import os
from pathlib import Path
import unittest


class CpStartupContractTest(unittest.TestCase):
    def test_loader_watchdog_period_fits_sdk_register(self):
        runfiles = Path(os.environ["TEST_SRCDIR"])
        defaults = list(runfiles.rglob("layouts/loader/cp.defaults"))
        self.assertEqual(1, len(defaults))
        values = dict(line.split("=", 1) for line in
                      defaults[0].read_text(encoding="utf-8").splitlines()
                      if line.startswith("CONFIG_") and "=" in line)
        # BK SDK rejects periods above WDT_F_PERIOD_V before enabling feeding.
        period = int(values["CONFIG_INT_WDT_PERIOD_MS"])
        self.assertGreater(period, 0)
        self.assertLessEqual(period, 0xffff)

if __name__ == "__main__":
    unittest.main()
