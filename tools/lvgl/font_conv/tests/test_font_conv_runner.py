"""Tests for the official LVGL font converter adapter."""

from pathlib import Path
import subprocess
import tempfile
import unittest

from python.runfiles import runfiles

import font_conv_runner


class FontConvRunnerTest(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        self.converter = runfiles.Create().Rlocation(
            "gizos/tools/lvgl/font_conv/fake_font_conv"
        )
        self.assertIsNotNone(self.converter)

    def tearDown(self) -> None:
        self.directory.cleanup()

    def _args(self, font_name: str = "font.ttf"):
        font = self.root / font_name
        font.write_bytes(b"font")
        source = self.root / "symbols.txt"
        source.write_text("中A\n文A\t", encoding="utf-8")
        output = self.root / "font.c"
        return font_conv_runner.parse_args(
            [
                "--converter",
                self.converter,
                "--font",
                str(font),
                "--symbol-source",
                str(source),
                "--range",
                "0x20-0x7E",
                "--size",
                "16",
                "--bpp",
                "4",
                "--font-name",
                "fixture_font_16",
                "--lv-include",
                "include/lvgl/lvgl.h",
                "--output",
                str(output),
            ]
        ), output

    def test_collects_stable_unique_non_control_symbols(self) -> None:
        args, output = self._args()
        font_conv_runner.convert(args)
        generated = output.read_text(encoding="utf-8")
        self.assertIn("symbols=U+0041,U+4E2D,U+6587", generated)
        self.assertIn("ranges=0x20-0x7E size=16 bpp=4", generated)
        self.assertIn("const lv_font_t fixture_font_16", generated)

    def test_rejects_invalid_utf8(self) -> None:
        args, _ = self._args()
        Path(args.symbol_source[0]).write_bytes(b"\xff")
        with self.assertRaises(UnicodeDecodeError):
            font_conv_runner.convert(args)

    def test_rejects_empty_effective_symbol_set(self) -> None:
        args, _ = self._args()
        Path(args.symbol_source[0]).write_text("\n\t", encoding="utf-8")
        args.ranges = []
        with self.assertRaisesRegex(ValueError, "requires symbols or ranges"):
            font_conv_runner.convert(args)

    def test_propagates_converter_failure_without_output(self) -> None:
        args, output = self._args("fail.ttf")
        with self.assertRaises(subprocess.CalledProcessError):
            font_conv_runner.convert(args)
        self.assertFalse(output.exists())

    def test_rejects_empty_converter_output(self) -> None:
        args, output = self._args("empty.ttf")
        with self.assertRaisesRegex(RuntimeError, "non-empty C source"):
            font_conv_runner.convert(args)
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
