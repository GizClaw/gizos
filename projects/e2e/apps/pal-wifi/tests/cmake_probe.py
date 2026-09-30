"""Bound host tool execution without waiting for inherited output pipe handles."""

import subprocess
import tempfile


def run_command(command, *, env=None):
    print("Overlay probe: " + subprocess.list2cmdline(command), flush=True)
    # Windows compiler/build servers can outlive CMake and inherit its stdout.
    # A file lets us wait for the command itself rather than pipe EOF.
    with tempfile.TemporaryFile(mode="w+", encoding="utf-8", errors="replace") as output:
        try:
            result = subprocess.run(command, env=env, stdout=output,
                                    stderr=subprocess.STDOUT, timeout=60)
        except subprocess.TimeoutExpired as error:
            output.seek(0)
            raise RuntimeError("Overlay command timed out: " +
                               subprocess.list2cmdline(command) + "\n" +
                               output.read()) from error
        output.seek(0)
        return subprocess.CompletedProcess(command, result.returncode,
                                           stdout=output.read(), stderr="")
