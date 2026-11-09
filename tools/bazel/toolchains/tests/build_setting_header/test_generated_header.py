"""The generated header carries exactly one canonical unsigned decimal define."""
from pathlib import Path
import sys
import unittest


class GeneratedHeaderTest(unittest.TestCase):
    def test_header(self):
        text = Path(sys.argv[1]).read_text()
        self.assertEqual(text, '#ifndef H2_FIXTURE_CYCLES_HEADER_INCLUDED\n#define H2_FIXTURE_CYCLES_HEADER_INCLUDED\n'
                               '#define H2_FIXTURE_CYCLES 10u\n#endif\n')


if __name__ == '__main__':
    unittest.main(argv=sys.argv[:1])
