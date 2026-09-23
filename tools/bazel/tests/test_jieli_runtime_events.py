"""Compile the Runtime/JieLi provider integration fixture with the host compiler."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
CORE = ROOT / 'native_component_src/jieli/wl82/h2_pal_core'


class RuntimeEventsTest(unittest.TestCase):
    def test_owner_threads(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'event-threads'
            subprocess.run([*shlex.split(os.environ.get('CC', 'cc')),
                            '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                            *shlex.split(os.environ.get('JIELI_TEST_CFLAGS', '')),
                            '-I', str(ROOT / 'libs/pal/include'),
                            '-I', str(ROOT / 'libs/atomic/include'), '-I', str(CORE / 'include'),
                            str(CORE / 'src/h2_jieli_wl82_platform_system_event.c'),
                            str(CORE / 'tests/src/test_jieli_wl82_event_threads.c'),
                            str(ROOT / 'libs/atomic/providers/c11/src/h2_atomic_c11.c'),
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=60)

    def test_launcher_then_runtime(self):
        sources = sorted((ROOT / 'libs/runtime/src').glob('*.c'))
        sources += sorted((ROOT / 'libs/pal/src/unsupported').glob('*.c'))
        sources += sorted((CORE / 'src').glob('h2_jieli_wl82_platform_*.c'))
        sources += [CORE / 'tests/src/h2_jieli_wl82_sdk_port_fake.c',
                    CORE / 'tests/src/test_jieli_wl82_runtime_events.c']
        includes = [ROOT / 'libs/runtime/include', ROOT / 'libs/pal/include',
                    ROOT / 'libs/atomic/include',
                    CORE / 'include', CORE / 'tests/include']
        sources += [ROOT / 'libs/atomic/providers/c11/src/h2_atomic_c11.c']
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'runtime-events'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            *[flag for path in includes for flag in ['-I', str(path)]],
                            *map(str, sources), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
