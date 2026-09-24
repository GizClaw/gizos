"""Compile the package pthread regression with the host C compiler (Linux GCC)."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
CORE = ROOT / "native_component_src/jieli/wl82/h2_pal_core"

if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as directory:
        binary = Path(directory) / "task-identity"
        subprocess.run([
            "cc", "-std=c11", "-pthread", "-Wall", "-Wextra", "-Werror",
            "-I" + str(ROOT / "libs/pal/include"),
            "-I" + str(ROOT / "libs/atomic/include"),
            "-I" + str(ROOT / "libs/atomic/providers/c11"),
            "-I" + str(CORE / "include"),
            str(CORE / "src/h2_jieli_wl82_platform_task.c"),
            str(CORE / "tests/src/test_jieli_wl82_task_identity.c"),
            str(ROOT / "libs/atomic/providers/c11/src/h2_atomic_c11.c"),
            "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True, timeout=20)
