import os
from pathlib import Path
import unittest


class CpStartupContractTest(unittest.TestCase):
    def test_shared_launcher_registers_transport_after_bk_init(self):
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
        transport = source.index("h2_bk_h2loader_cp_transport_start()", policy)
        self.assertLess(policy, transport)

        transports = list(
            runfiles.rglob("h2_cp_transport/src/h2_bk_h2loader_cp_transport.c")
        )
        self.assertEqual(1, len(transports), [str(path) for path in transports])
        transport = transports[0].read_text(encoding="utf-8")
        self.assertIn("psram_malloc(H2_BK_CP_UART_RX_QUEUE_SIZE)", transport)
        self.assertIn("rtos_create_psram_thread(", transport)
        self.assertIn("bk_uart_take_rx_isr(s_uart_id, uart_rx_isr, NULL)", transport)
        self.assertIn("bk_uart_disable_rx_interrupt(s_uart_id)", transport)
        self.assertIn("uart_rx_isr(s_uart_id, NULL)", transport)
        self.assertIn("H2_BK_CP_READY_REQUEST", transport)
        self.assertIn("SHELL_IO_CTRL_TX_SUSPEND", transport)
        self.assertIn("bk_uart_write_bytes(s_uart_id, data", transport)
        self.assertIn("SHELL_IO_CTRL_TX_RESUME", transport)
        self.assertNotIn("H2_BK_CP_HOST_FRAME", transport)


if __name__ == "__main__":
    unittest.main()
