import tempfile
import unittest
from pathlib import Path

from fontTools.ttLib import TTLibError

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

    def test_header_guard_does_not_collide_for_case_distinct_symbols(self) -> None:
        lowercase = ttf_subset.render_header("fixture_font").splitlines()[0]
        uppercase = ttf_subset.render_header("FIXTURE_FONT").splitlines()[0]
        self.assertNotEqual(lowercase, uppercase)

    def test_collect_codepoints_filters_controls(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "symbols.txt"
            source.write_text("A\n中A", encoding="utf-8")
            self.assertEqual(
                ttf_subset.collect_codepoints([source], []),
                frozenset([ord("A"), ord("中")]),
            )

    def test_collect_codepoints_rejects_malformed_utf8(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "symbols.txt"
            source.write_bytes(b"\xff")
            with self.assertRaises(UnicodeDecodeError):
                ttf_subset.collect_codepoints([source], [])

    def test_collect_codepoints_rejects_empty_effective_set(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "symbols.txt"
            source.write_text("\n\t", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "requires symbols or ranges"):
                ttf_subset.collect_codepoints([source], [])

    def test_subset_font_rejects_invalid_font(self) -> None:
        with self.assertRaises(TTLibError):
            ttf_subset.subset_font(b"not a font", frozenset([ord("A")]))

    def test_validate_codepoints_reports_missing_glyphs(self) -> None:
        with self.assertRaisesRegex(RuntimeError, r"U\+4E2D"):
            ttf_subset.validate_codepoints(frozenset([ord("中")]), frozenset())


if __name__ == "__main__":
    unittest.main()
