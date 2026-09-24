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

    def test_independent_status_facts_are_inline(self):
        day = EVIDENCE / '2026-09-14'
        uart = (day / 'loader-uart-lifecycle.md').read_text()
        ble = (day / 'loader-ble-lifecycle.md').read_text()
        snapshots = [
            ('after_install', '1', '0',
             '42c39b7aae00917e44e9807503fe57fdb4ccad25b58793ab12087a2da03d22cd',
             '42c39b7aae00917e44e9807503fe57fdb4ccad25b58793ab12087a2da03d22cd'),
            ('before_install', '1', '1',
             '7130cfe2386c86a7dcf82ccd15854f14b64fb525be14a984cfdaa5dd19d64dfa',
             'fec8c47945b29ac9f294d177d507d19705195c34ba8f76c63a9d60d5ff95cfb7'),
            ('ble_after_run_1', '1', '1',
             '42c39b7aae00917e44e9807503fe57fdb4ccad25b58793ab12087a2da03d22cd',
             'a75bdeca184ecc78ebdf5126dfc75df3170976dbe4a4fcfddfa7da363c3ac93e'),
            ('ble_after_run_2', '1', '1',
             '42c39b7aae00917e44e9807503fe57fdb4ccad25b58793ab12087a2da03d22cd',
             'a75bdeca184ecc78ebdf5126dfc75df3170976dbe4a4fcfddfa7da363c3ac93e'),
            ('ble_before', '1', '1',
             '42c39b7aae00917e44e9807503fe57fdb4ccad25b58793ab12087a2da03d22cd',
             'a75bdeca184ecc78ebdf5126dfc75df3170976dbe4a4fcfddfa7da363c3ac93e'),
            ('final', '1', '1',
             '42c39b7aae00917e44e9807503fe57fdb4ccad25b58793ab12087a2da03d22cd',
             'a75bdeca184ecc78ebdf5126dfc75df3170976dbe4a4fcfddfa7da363c3ac93e'),
            ('initial', '1', '1',
             '7130cfe2386c86a7dcf82ccd15854f14b64fb525be14a984cfdaa5dd19d64dfa',
             'fec8c47945b29ac9f294d177d507d19705195c34ba8f76c63a9d60d5ff95cfb7'),
        ]
        for name, partition, stage, p1, p2 in snapshots:
            doc = ble if name.startswith('ble_') else uart
            with self.subTest(snapshot=name):
                for fact in (f'running_partition={partition}', f'stage_valid={stage}', p1, p2):
                    self.assertIn(fact, doc)
        # Frozen acceptance assertions must not depend on a captured JSON report.
        for doc in (uart, ble):
            self.assertIn('running_partition=1', doc)
            self.assertIn('stage_valid=0', doc)
            self.assertIn('last_result=0', doc)
            self.assertRegex(doc, r'[0-9a-f]{64}')
        final = (EVIDENCE / '2026-09-15' / 'pal-final-acceptance.md').read_text()
        for fact in ('5698d9ad9935073970857040c0986fec4c678f26f6fffbfff9ebbeb5b1088ada',
                     '9f3eea5602a5919f65dd17d3f293bf5b08e1b1a428c0298da3ac11e59fc5ecae',
                     'result=0 passed=10 failed=0', '331.305', '371.637', '366.230',
                     'running_partition=1', 'stage_valid=0', 'last_result=0'):
            self.assertIn(fact, final)
        # The oldest reports have no pre-existing sibling page; their summaries
        # must retain the failure totals as well as later successful checkpoints.
        for date in ('2026-09-12', '2026-09-13'):
            summary = (EVIDENCE / date / 'lifecycle-summary.md').read_text()
            for field in ('sha256', 'summary.cases', 'summary.passed', 'summary.failed',
                          'running_partition=', 'stage_valid=', 'last_result='):
                self.assertIn(field, summary)
        self.assertIn('stage_valid=0', uart)
        self.assertIn('25/25', uart)
        self.assertIn('22/22', ble)


if __name__ == '__main__':
    unittest.main()
