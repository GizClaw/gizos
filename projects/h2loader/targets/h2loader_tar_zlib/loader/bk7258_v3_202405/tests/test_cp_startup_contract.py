import os
from pathlib import Path
import unittest


class CpStartupContractTest(unittest.TestCase):
    def test_shared_launcher_starts_transport_without_waiting_for_bk_init(self):
        runfiles = Path(os.environ["TEST_SRCDIR"])
        launchers = list(runfiles.rglob("h2loader_cp_launcher/src/cp_main.c"))

        self.assertEqual(1, len(launchers), [str(path) for path in launchers])
        source = launchers[0].read_text(encoding="utf-8")
        main = source.index("int main(void)")
        create = source.index("rtos_create_thread(", main)
        initialize = source.index("return bk_init();", main)
        self.assertLess(create, initialize)
        self.assertIn("H2_BK_CP_BOOTSTRAP_STACK_SIZE 1024u", source)
        self.assertIn('"h2-cp-bootstrap"', source)
        self.assertNotIn("s_bk_init_done", source)
        self.assertNotIn("rtos_init_semaphore(", source)

        transports = list(
            runfiles.rglob("h2_cp_transport/src/h2_bk_h2loader_cp_transport.c")
        )
        self.assertEqual(1, len(transports), [str(path) for path in transports])
        transport = transports[0].read_text(encoding="utf-8")
        self.assertIn("psram_malloc(H2_BK_CP_UART_RX_QUEUE_SIZE)", transport)
        self.assertIn("rtos_create_psram_thread(", transport)


if __name__ == "__main__":
    unittest.main()
