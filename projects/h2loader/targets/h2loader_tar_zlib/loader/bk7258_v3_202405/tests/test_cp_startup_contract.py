import os
from pathlib import Path
import unittest


class CpStartupContractTest(unittest.TestCase):
    def test_launchers_use_sdk_app_lifecycle(self):
        runfiles = Path(os.environ["TEST_SRCDIR"])
        launchers = sorted(
            path
            for path in runfiles.rglob("cp_main.c")
            if "bk7258_v3_202405/cp/cp_main.c" in path.as_posix()
        )

        self.assertEqual(11, len(launchers), [str(path) for path in launchers])
        for launcher in launchers:
            source = launcher.read_text(encoding="utf-8")
            with self.subTest(launcher=launcher):
                main = source.index("int main(void)")
                register = source.index("rtos_set_user_app_entry(", main)
                initialize = source.index("return bk_init();", main)
                self.assertLess(register, initialize)
                self.assertNotIn("s_bk_init_done", source)
                self.assertNotIn("h2_bk_cp_start_task", source)
                self.assertNotIn('"h2-cp-start"', source)
                self.assertNotIn("rtos_init_semaphore(", source)
                self.assertNotIn("rtos_create_thread(", source)


if __name__ == "__main__":
    unittest.main()
