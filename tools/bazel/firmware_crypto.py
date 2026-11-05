"""Runtime-only secret descriptors and diagnostics for firmware transforms."""

import os
import sys


def message(text: str, *, error: bool = False) -> None:
    """Diagnostics cannot change publication state or expose an I/O exception."""
    stream = sys.stderr if error else sys.stdout
    if stream is None:
        return
    try:
        print(text, file=stream, flush=True)
    except (OSError, ValueError):
        try:
            with open(os.devnull, "w") as sink:
                os.dup2(sink.fileno(), stream.fileno())
        except (OSError, ValueError, AttributeError):
            if error:
                sys.stderr = None
            else:
                sys.stdout = None


def protect_process() -> None:
    """Establish crash-dump protection before accepting secret bytes."""
    import resource

    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    if sys.platform == "linux":
        import ctypes

        try:
            prctl = ctypes.CDLL(None, use_errno=True).prctl
        except AttributeError:
            raise ValueError("DUMP_PROTECTION_UNAVAILABLE") from None
        prctl.argtypes = [ctypes.c_int] + [ctypes.c_ulong] * 4
        prctl.restype = ctypes.c_int
        if prctl(4, 0, 0, 0, 0) != 0 or prctl(3, 0, 0, 0, 0) != 0:
            raise ValueError("DUMP_PROTECTION_FAILED")


def read_key(fd: int, timeout: float = 30.0) -> bytearray:
    """Read exactly 32 raw bytes and EOF from a pipe or socket by one deadline."""
    import selectors
    import stat
    import time

    mode = os.fstat(fd).st_mode
    if not (stat.S_ISFIFO(mode) or stat.S_ISSOCK(mode)):
        raise ValueError("KEY_REQUIRES_PIPE_OR_SOCKET")
    key = bytearray()
    try:
        deadline = time.monotonic() + timeout
        with os.fdopen(os.dup(fd), "rb", buffering=0) as secret, selectors.DefaultSelector() as readiness:
            readiness.register(secret, selectors.EVENT_READ)
            while len(key) < 33:
                remaining = deadline - time.monotonic()
                if remaining <= 0 or not readiness.select(remaining):
                    raise ValueError("KEY_INPUT_TIMEOUT")
                chunk = os.read(secret.fileno(), 33 - len(key))
                if not chunk:
                    break
                key.extend(chunk)
        if len(key) != 32:
            raise ValueError("KEY_LENGTH")
        return key
    except BaseException:
        key[:] = b"\0" * len(key)
        raise
