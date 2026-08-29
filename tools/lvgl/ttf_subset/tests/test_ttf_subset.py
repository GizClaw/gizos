import tempfile
import unittest
from pathlib import Path

import ttf_subset


class TtfSubsetTest(unittest.TestCase):
    def test_range_is_inclusive(self) -> None:
        self.assertEqual(list(ttf_subset.parse_range("0x41-0x43")), [65, 66, 67])

    def test_rejects_reversed_range(self) -> None:
        with self.assertRaisesRegex(ValueError, "invalid Unicode range"):
            ttf_subset.parse_range("0x43-0x41")

    def test_header_exports_data_and_size(self) -> None:
        header = ttf_subset.render_header("fixture_font")
        self.assertIn("fixture_font_data[]", header)
        self.assertIn("fixture_font_size", header)

    def test_collect_codepoints_filters_controls(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "symbols.txt"
            source.write_text("A\n中A", encoding="utf-8")
            self.assertEqual(
                ttf_subset.collect_codepoints([source], []),
                frozenset([ord("A"), ord("中")]),
            )


if __name__ == "__main__":
    unittest.main()
