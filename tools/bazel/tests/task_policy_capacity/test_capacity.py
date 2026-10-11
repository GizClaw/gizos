"""Regress the static pool for real generated policies with shared paths."""

from pathlib import Path
import re
import sys
import unittest


class CapacityTest(unittest.TestCase):
    def test_shared_prefix_storage(self):
        # Root + fixture/ + load + save + nested/ + leaf = 28 nodes.
        # The exact fixture route and the nested prefix add no separate nodes.
        for path in sys.argv[1:3]:
            with self.subTest(path=path):
                source = Path(path).read_text()
                capacity = re.search(r"ROUTE_NODE_CAPACITY\s*=\s*([^,]+),", source)
                self.assertIsNotNone(capacity)
                self.assertEqual(capacity[1], "28u")
                self.assertIn("H2_TRIE_ROUTE_PREFIX", source)
                self.assertIn("H2_TRIE_ROUTE_EXACT", source)

    def test_unicode_preserves_byte_bound(self):
        source = Path(sys.argv[3]).read_text()
        capacity = re.search(r"ROUTE_NODE_CAPACITY\s*=\s*([^,]+),", source)
        self.assertIsNotNone(capacity)
        self.assertIn("H2_TRIE_LITERAL_NODE_COUNT", capacity[1])


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
