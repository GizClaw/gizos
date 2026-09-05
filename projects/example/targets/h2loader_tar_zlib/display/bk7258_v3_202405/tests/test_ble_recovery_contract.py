import os
from pathlib import Path
import unittest


class BleRecoveryContractTest(unittest.TestCase):
    def test_ble_failure_falls_through_to_uart_recovery_and_rendering(self):
        runfiles = Path(os.environ["TEST_SRCDIR"])
        launchers = list(
            runfiles.rglob(
                "display/bk7258_v3_202405/ap/ap_main.c"
            )
        )

        self.assertEqual(1, len(launchers), [str(path) for path in launchers])
        source = launchers[0].read_text(encoding="utf-8")
        ble_start = source.index("h2_bk_h2loader_start_app_ble(")
        ble_failure = source.index("if (rc != H2_PAL_OK)", ble_start)
        recovery = source.index("recovery=uart", ble_failure)
        failure_block_end = source.index("\n    }", recovery)
        run_marker = source.index("stage=run_begin", failure_block_end)
        render = source.index("h2_smoke_display_run(runtime)", run_marker)

        failure_block = source[ble_failure:failure_block_end]
        self.assertNotIn("reboot_to_loader", failure_block)
        self.assertLess(recovery, run_marker)
        self.assertLess(run_marker, render)


if __name__ == "__main__":
    unittest.main()
