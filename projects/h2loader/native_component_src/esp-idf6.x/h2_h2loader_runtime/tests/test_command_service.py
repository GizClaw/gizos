"""Exercise the native USB serve loop with optional service hook failures."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[6]
COMPONENT = ROOT / "projects/h2loader/native_component_src/esp-idf6.x/h2_h2loader_runtime"


def function(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class CommandService(unittest.TestCase):
    def test_usb_recovery_and_cleanup_lifetimes(self):
        source = (COMPONENT / "src/h2_esp_h2loader_runtime.c").read_text()
        functions = "\n".join(function(source, signature) for signature in [
            "static int serve_loader(",
            "static int stop_serve_loader(",
        ])
        fixture = (COMPONENT / "tests/command_service_fixture.c").read_text()
        with tempfile.TemporaryDirectory() as directory:
            unit = Path(directory) / "test.c"
            binary = Path(directory) / "test"
            unit.write_text(fixture.replace("/* FUNCTIONS */", functions))
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I" + str(ROOT / "libs/pal/include"), str(unit),
                "-o", str(binary),
            ], check=True)
            for case in ["no-session", "usb-session", "activate", "early-stop", "defaults"]:
                with self.subTest(case=case):
                    result = subprocess.run([str(binary), case], capture_output=True,
                                            text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(result.stdout.count("command_service_stop_failed"),
                                     0 if case == "defaults" else 2)
                    self.assertEqual(result.stdout.count("command_service_poll_failed"),
                                     0 if case in ["defaults", "early-stop"] else 1)


if __name__ == "__main__":
    unittest.main()
