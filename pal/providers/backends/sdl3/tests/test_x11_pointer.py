"""Drive the production SDL provider on a private Xvfb display."""
import ctypes
import os
from pathlib import Path
import select
import subprocess
import sys


def send_clicks(client):
    x11 = ctypes.CDLL('libX11.so.6')
    xtst = ctypes.CDLL('libXtst.so.6')
    x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
    x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
    xtst.XTestFakeButtonEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint,
                                         ctypes.c_int, ctypes.c_ulong]
    display = x11.XOpenDisplay(None)
    assert display
    try:
        with subprocess.Popen([client], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              text=True, env={**os.environ, 'SDL_VIDEODRIVER': 'x11'}) as proc:
            try:
                for stage, edges in [('old clicks and held press', (1, 0, 1, 0, 1)),
                                     ('release', (0,)), ('first click after idle restart', (1, 0))]:
                    if not select.select([proc.stdout], [], [], 5)[0]:
                        raise TimeoutError(stage)
                    assert proc.stdout.readline().strip() == stage, stage
                    for down in edges:
                        assert xtst.XTestFakeButtonEvent(display, 1, down, 0)
                    x11.XSync(display, 0)
                    proc.stdin.write('\n')
                    proc.stdin.flush()
                assert proc.wait(timeout=8) == 0
                print(proc.stdout.read(), end='')
            finally:
                if proc.poll() is None:
                    proc.kill()
    finally:
        x11.XCloseDisplay(display)


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--drive':
        send_clicks(sys.argv[2])
    else:
        subprocess.run(['xvfb-run', '-a', '-e', '/dev/stderr', '-s', '-screen 0 640x480x24 -nolisten tcp',
                        sys.executable, __file__, '--drive', str(Path(sys.argv[1]).resolve())],
                       timeout=25, check=True)
