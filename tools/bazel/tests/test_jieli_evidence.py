"""Keep committed AC791N evidence reviewable without raw captures or local drafts."""
from pathlib import Path
import re
import unittest
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[3]
EVIDENCE = ROOT / 'guides/apps/h2loader/boards/jieli_ac791n_devkit/evidence'
# No retained JSON is a build/test input. Future exceptions must name an owned
# fixture and its executable consumer here; captured run output is never allowed.
JSON_INPUTS = frozenset()


class EvidenceTest(unittest.TestCase):
    def test_owned_inputs_only(self):
        rejected = [str(p.relative_to(EVIDENCE)) for p in EVIDENCE.rglob('*')
                    if p.is_file() and (p.suffix.lower() in ('.log', '.status', '.orig', '.rej')
                    or (p.suffix.lower() == '.json' and str(p.relative_to(EVIDENCE)) not in JSON_INPUTS))]
        self.assertEqual(sorted(rejected), [], 'Captured outputs do not belong in implementation evidence')
        for path in EVIDENCE.rglob('*.md'):
            with self.subTest(path=path.name):
                text = path.read_text()
                # The closing label also matches images: ![alt](target).
                targets = re.findall(r'\]\(((?:[^()\n]|\([^()\n]*\))+)\)', text)
                targets += re.findall(r'^ {0,3}\[[^]\n]+\]:[ \t]*(.+)$', text, re.MULTILINE)
                for target in targets:
                    target = re.sub(r"\s+(?:\"[^\"]*\"|'[^']*')\s*$", '', target).strip()
                    if target.startswith('<') and target.endswith('>'):
                        target = target[1:-1]
                    suffix = Path(urlsplit(target).path).suffix.lower()
                    self.assertNotIn(suffix, ('.log', '.status', '.json', '.orig', '.rej'))

if __name__ == '__main__':
    unittest.main()
