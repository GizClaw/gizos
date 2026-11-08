"""Resume real device frame parsers after a callback consumes its deadline."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[4]
SOURCES = [
    ('libs/h2loader/device/src/h2_device_serial.c', 'poll_physical'),
]


def function(source, name):
    match = re.search(r'^(?:static\s+)?(?:int|h2_pal_result_t)\s+' +
                      re.escape(name) + r'\s*\(', source, re.MULTILINE)
    if match is None:
        raise ValueError('missing poll implementation: ' + name)
    start = match.start()
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end].replace(name, 'poll_physical', 1)


class BufferedPoll(unittest.TestCase):
    def test_resume(self):
        frame = (ROOT / 'libs/net/iostreamikcp/src/h2_iostreamikcp_frame.c').read_text()
        frame = frame.replace('#include "h2_iostreamikcp_internal.h"',
            '#include "h2_iostreamikcp.h"\n#define H2_IOSTREAMIKCP_FRAME_MAGIC_LEN 6u\n#define H2_IOSTREAMIKCP_FRAME_LEN_OFFSET 12u')
        fixture = (ROOT / 'libs/h2loader/device/tests/fixtures/h2loader_buffered_poll.c').read_text()
        with tempfile.TemporaryDirectory() as directory:
            unit = Path(directory) / 'test.c'
            binary = Path(directory) / 'test'
            for source, name in SOURCES:
                text = (ROOT / source).read_text()
                unit.write_text(fixture.replace('/* FRAME */', frame).replace('/* POLL */', function(text, name)))
                subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(ROOT / 'pal/include'),
                    '-I' + str(ROOT / 'libs/net/iostreamikcp/include'), str(unit), '-o', str(binary)], check=True)
                for case in ['timeout', 'would_block', 'read_error', 'partial']:
                    with self.subTest(source=source, case=case):
                        result = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=10)
                        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
