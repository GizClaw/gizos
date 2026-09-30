"""Bound host tool execution without waiting for inherited output pipe handles."""

import os
from pathlib import Path
import subprocess
import tempfile


def run_command(command, *, env=None):
    if os.name == "nt" and "-S" in command:
        command = [*command, "--trace-expand"]
    print("Overlay probe: " + subprocess.list2cmdline(command), flush=True)
    # Windows compiler/build servers can outlive CMake and inherit its stdout.
    # A file lets us wait for the command itself rather than pipe EOF.
    with tempfile.TemporaryFile(mode="w+", encoding="utf-8", errors="replace") as output:
        with subprocess.Popen(command, env=env, stdout=output,
                              stderr=subprocess.STDOUT) as process:
            try:
                process.wait(timeout=60)
            except subprocess.TimeoutExpired as error:
                try:
                    if os.name == "nt":
                        subprocess.run(["taskkill", "/F", "/T", "/PID", str(process.pid)],
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                       timeout=10)
                finally:
                    process.kill()
                    process.wait(timeout=10)
                output.seek(0)
                diagnostic = output.read()[-12000:]
                if "-B" in command:
                    log = Path(command[command.index("-B") + 1]) / "CMakeFiles/CMakeConfigureLog.yaml"
                    if log.is_file():
                        diagnostic += "\nConfigure log:\n" + log.read_text(errors="replace")[-12000:]
                raise RuntimeError("Overlay command timed out: " +
                                   subprocess.list2cmdline(command) + "\n" +
                                   diagnostic) from error
        output.seek(0)
        return subprocess.CompletedProcess(command, process.returncode,
                                           stdout=output.read(), stderr="")
