"""Exercise real session admission under allocation and ACK failures."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[4]
OWNER = ROOT / "libs/h2loader/device"


def function(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class SessionAdmission(unittest.TestCase):
    def test_failure_and_reconnect(self):
        source = (OWNER / "src/h2_device_serial.c").read_text()
        code = "\n".join(function(source, signature) for signature in [
            "void h2_device_serial_deactivate(",
            "h2_pal_result_t h2_device_serial_activate_pending(",
            "static int app_read_byte(",
        ])
        fixture = (OWNER / "tests/fixtures/serial_session_admission.c").read_text()
        with tempfile.TemporaryDirectory() as directory:
            unit = Path(directory) / "test.c"
            binary = Path(directory) / "test"
            unit.write_text(fixture.replace("/* FUNCTIONS */", code))
            for compact in [False, True]:
                defines = []
                if compact:
                    defines = ["-DTEST_COMPACT=1", "-DTEST_STREAM_RX_BUFFER=1024",
                               "-DTEST_RECEIVE_WINDOW=8"]
                subprocess.run([
                    "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", *defines,
                    "-I" + str(ROOT / "pal/include"),
                    "-I" + str(ROOT / "libs/net/iostreamikcp/include"),
                    str(unit), "-o", str(binary),
                ], check=True)
                for case in ["oom", "ack", "flood", "success", "invalid"]:
                    with self.subTest(compact=compact, case=case):
                        result = subprocess.run([str(binary), case], capture_output=True,
                                                text=True, timeout=10)
                        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
