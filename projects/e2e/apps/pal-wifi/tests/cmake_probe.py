"""Bound host tool execution without waiting for inherited output pipe handles."""

import os
from functools import lru_cache
from pathlib import Path
import shutil
import subprocess
import tempfile


@lru_cache(maxsize=1)
def windows_compiler_environment():
    program_files = Path(os.environ.get("ProgramFiles", "C:/Program Files"))
    candidates = []
    if os.environ.get("VSINSTALLDIR"):
        candidates.append(Path(os.environ["VSINSTALLDIR"]) / "Common7/Tools/VsDevCmd.bat")
    candidates.extend(sorted((program_files / "Microsoft Visual Studio").glob(
        "*/*/Common7/Tools/VsDevCmd.bat"), key=lambda path: path.stat().st_mtime, reverse=True))
    script = next((path for path in candidates if path.is_file()), None)
    if script is None:
        raise RuntimeError("The Windows overlay probe requires Visual C++ Build Tools")
    # Print only compiler search variables, never the whole inherited environment.
    setup = (f'call "{script}" -no_logo -arch=x64 -host_arch=x64 >nul && '
             '(set PATH & set INCLUDE & set LIB)')
    result = run_command(setup, shell=True)
    if result.returncode != 0:
        raise RuntimeError("Visual C++ environment setup failed: " + result.stdout)
    environment = {}
    for line in result.stdout.splitlines():
        key, separator, value = line.partition("=")
        if separator and key.upper() in {"PATH", "INCLUDE", "LIB", "LIBPATH"}:
            environment[key.upper()] = value
    if not all(environment.get(key) for key in ("PATH", "INCLUDE", "LIB")):
        raise RuntimeError("Visual C++ environment is incomplete")
    ninja = shutil.which("ninja", path=environment["PATH"])
    if ninja is None:
        bundled = script.parents[2] / "Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"
        if not bundled.is_file():
            raise RuntimeError("The Windows overlay probe requires Ninja")
        environment["PATH"] = str(bundled.parent) + os.pathsep + environment["PATH"]
    return environment


def run_command(command, *, env=None, shell=False):
    if not isinstance(command, str) and os.name == "nt" and Path(command[0]).name.lower() == "cmake.exe":
        env = dict(os.environ if env is None else env, **windows_compiler_environment())
        if "-S" in command:
            command = [*command, "-G", "Ninja"]
    display = command if isinstance(command, str) else subprocess.list2cmdline(command)
    print("Overlay probe: " + display, flush=True)
    # Windows compiler/build servers can outlive CMake and inherit its stdout.
    # A file lets us wait for the command itself rather than pipe EOF.
    with tempfile.TemporaryFile(mode="w+", encoding="utf-8", errors="replace") as output:
        with subprocess.Popen(command, env=env, shell=shell, stdout=output,
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
                if not isinstance(command, str) and "-B" in command:
                    log = Path(command[command.index("-B") + 1]) / "CMakeFiles/CMakeConfigureLog.yaml"
                    if log.is_file():
                        diagnostic += "\nConfigure log:\n" + log.read_text(errors="replace")[-12000:]
                raise RuntimeError("Overlay command timed out: " +
                                   display + "\n" +
                                   diagnostic) from error
        output.seek(0)
        return subprocess.CompletedProcess(command, process.returncode,
                                           stdout=output.read(), stderr="")
