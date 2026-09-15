"""Resume real device frame parsers after a callback consumes its deadline."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SOURCES = [
    'projects/h2loader/native_component_src/esp-idf6.x/h2_h2loader_runtime/src/h2_esp_h2loader_iostreamikcp.c',
    'projects/h2loader/native_component_src/bk7258/ap/h2loader_bk/src/h2_bk_h2loader_iostreamikcp.c',
]


def function(source, name):
    start = source.index(name + '(')
    start = source.rfind('static ', 0, start)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end].replace('transport_poll_physical(', 'poll_physical(')


class BufferedPoll(unittest.TestCase):
    def test_resume(self):
        frame = (ROOT / 'libs/iostreamikcp/src/h2_iostreamikcp_frame.c').read_text()
        frame = frame.replace('#include "h2_iostreamikcp_internal.h"',
            '#include "h2_iostreamikcp.h"\n#define H2_IOSTREAMIKCP_FRAME_MAGIC_LEN 6u\n#define H2_IOSTREAMIKCP_FRAME_LEN_OFFSET 12u')
        fixture = (ROOT / 'tools/bazel/tests/fixtures/h2loader_buffered_poll.c').read_text()
        with tempfile.TemporaryDirectory() as directory:
            unit = Path(directory) / 'test.c'
            binary = Path(directory) / 'test'
            for source in SOURCES:
                text = (ROOT / source).read_text()
                name = 'transport_poll_physical' if 'esp-idf' in source else 'poll_physical'
                unit.write_text(fixture.replace('/* FRAME */', frame).replace('/* POLL */', function(text, name)))
                subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(ROOT / 'libs/pal/include'),
                    '-I' + str(ROOT / 'libs/iostreamikcp/include'), str(unit), '-o', str(binary)], check=True)
                for case in ['timeout', 'would_block', 'read_error', 'partial']:
                    with self.subTest(source=source, case=case):
                        result = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=10)
                        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
