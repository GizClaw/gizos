import os
from pathlib import Path
import unittest


class CpStartupContractTest(unittest.TestCase):
    def test_managed_uart_uses_one_460800_contract(self):
        runfiles = Path(os.environ["TEST_SRCDIR"])
        definitions = {
            "h2_bk_platform_uart_io_stream.c": "config->baud_rate != CONFIG_UART_PRINT_BAUD_RATE",
            "h2_bk_h2loader_iostreamikcp.c": "#define H2_BK_SERIAL_BAUD_RATE 460800u",
        }
        for name, expected in definitions.items():
            sources = list(runfiles.rglob(name))
            self.assertEqual(1, len(sources), [str(path) for path in sources])
            self.assertIn(expected, sources[0].read_text(encoding="utf-8"))

        defaults = list(runfiles.rglob("ap.defaults"))
        self.assertEqual(4, len(defaults), [str(path) for path in defaults])
        for path in defaults:
            config = path.read_text(encoding="utf-8")
            self.assertIn("CONFIG_UART_PRINT_BAUD_RATE=460800", config)
            self.assertIn("CONFIG_UART_PRINT_PORT=1", config)
            self.assertIn("CONFIG_SYS_PRINT_DEV_UART=y", config)
            self.assertIn("# CONFIG_SYS_PRINT_DEV_MAILBOX is not set", config)

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

    def test_shared_launcher_preserves_sdk_boot_without_uart_transport(self):
        runfiles = Path(os.environ["TEST_SRCDIR"])
        launchers = list(runfiles.rglob("h2loader_cp_launcher/src/cp_main.c"))

        self.assertEqual(1, len(launchers), [str(path) for path in launchers])
        source = launchers[0].read_text(encoding="utf-8")
        main = source.index("int main(void)")
        register = source.index("rtos_set_user_app_entry(", main)
        initialize = source.index("return bk_init();", register)
        self.assertLess(register, initialize)
        self.assertNotIn("rtos_create_thread(", source)
        entry = source.index("static void h2loader_cp_entry(void)")
        policy = source.index("h2_bk_target_task_policy_install()", entry)
        boot = source.index("bk_pm_module_vote_boot_cp1_ctrl(", policy)
        self.assertLess(policy, boot)
        self.assertNotIn("bk_uart_", source)

        launchers = list(runfiles.rglob("bk7258_v3_202405/ap/ap_main.c"))
        self.assertEqual(1, len(launchers), [str(path) for path in launchers])
        launcher = launchers[0].read_text(encoding="utf-8")
        # The entry task takes its bk/h2loader policy from the target table.
        self.assertIn("h2_bk_platform_task_api(), &entry_options", launcher)
        self.assertNotIn("rtos_create_thread(", launcher)
        probe = launcher.index("static int h2loader_probe_pref(void)")
        namespace_missing = launcher.index(
            "if (rc == H2_PAL_ERR_NOT_FOUND) return H2_PAL_OK;", probe
        )
        generic_failure = launcher.index("if (rc != H2_PAL_OK) return rc;", probe)
        self.assertLess(namespace_missing, generic_failure)


if __name__ == "__main__":
    unittest.main()
