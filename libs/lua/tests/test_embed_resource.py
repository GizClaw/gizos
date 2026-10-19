import importlib.util
import pathlib
import subprocess
import sys
import tempfile
import unittest


_PATH = pathlib.Path(__file__).resolve().parents[1] / "embed_resource.py"
_SPEC = importlib.util.spec_from_file_location("embed_resource", _PATH)
_MODULE = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(_MODULE)
compact = _MODULE.compact_source


class CompactSourceTest(unittest.TestCase):
    def test_comments_leave_token_separators(self):
        self.assertEqual(compact(b"return--[[comment]]1 -- tail"), b"return 1")
        self.assertEqual(compact(b"a - --[[ gap ]] - b"), b"a - - b")
        self.assertEqual(compact(b"a[ --[[ gap ]] [1] ]"), b"a[ [1] ]")
        self.assertEqual(compact(b"return 1 .. -- comment\n .5"), b"return 1 ..\n.5")

    def test_numbers_and_adjacent_tokens_are_not_rewritten(self):
        for source in (b"return a - -b", b"return a..b", b"return 1 .. 2",
                       b"return 1..2", b"return 0x1p4, 1e-3, .5, 0xe+1",
                       b"return 1 e3", b"return 0x1 p4", b"return 1 . . 2",
                       b"return a / / b", b"return a < < b"):
            with self.subTest(source=source):
                self.assertEqual(compact(source), source)
        for left, right in ((b"1", b"e3"), (b"0x1", b"p4"), (b".", b"."),
                            (b"-", b"-"), (b"[", b"["), (b"/", b"/"),
                            (b"<", b"<"), (b"=", b"="), (b":", b":")):
            with self.subTest(left=left, right=right):
                self.assertEqual(compact(left + b" --[[gap]]" + right),
                                 left + b" " + right)

    def test_quoted_strings_are_verbatim(self):
        for literal in (b"'-- not a comment'", b'"x\\\" -- y"',
                        b"'x\\\\'", b'"a\\z \t\r\n  b"', b'"a\\\n b"',
                        br'"\x22\x5c\u{22}\u{1F642}\034\092\000\255"',
                        br'"\1x\12x\1234"', b'"a\\z \t\v\f\r\n\n\r b"',
                        "'中文 -- 文本'".encode()):
            self.assertEqual(compact(b"  return  " + literal), b"return " + literal)

    def test_long_strings_and_comment_delimiters(self):
        for literal in (b"[[\n  -- hello\n]]", b"[=[a ]] -- b]=]",
                        b"[==[a ]=] b]==]", b"[====[\r\n x]====]"):
            self.assertEqual(compact(b"return " + literal), b"return " + literal)
        self.assertEqual(compact(b"a--[==[ ]=]\n \r\n]==]b"), b"a\n\nb")
        self.assertEqual(compact(b"a--[=not-long\nb"), b"a\nb")
        self.assertEqual(compact(b"a--[ 'not a string\nb"), b"a\nb")
        for level in (0, 1, 2, 8, 128):
            opening = b"[" + b"=" * level + b"["
            closing = b"]" + b"=" * level + b"]"
            wrong_close = b"]" + b"=" * (level + 1) + b"]"
            literal = opening + b"-- ' \r\n" + wrong_close + b" " + closing
            with self.subTest(level=level):
                self.assertEqual(compact(b"return " + literal), b"return " + literal)
                self.assertEqual(compact(b"a--" + literal + b"b"), b"a\nb")

    def test_line_numbers_do_not_merge_newlines(self):
        self.assertEqual(compact(b"a\r\nb\n\rc\rd\ne"), b"a\nb\nc\nd\ne")
        self.assertEqual(compact(b"a\r \nb\n-- tail\rc"), b"a\n\nb\n\nc")
        self.assertEqual(compact(b"a\r--[[\n]]b"), b"a\n\nb")
        self.assertEqual(compact(b"a--[=[\r\n\n\r\r]=]b"), b"a\n\n\nb")

    def test_whitespace_and_idempotence(self):
        original = b" \tlocal  x =  1\t -- test\n\n  return x \t\n"
        result = b"local x = 1\n\nreturn x\n"
        self.assertEqual(compact(original), result)
        self.assertEqual(compact(result), result)
        self.assertEqual(compact(b" \t-- all comment"), b"")

    def test_shebang_is_not_a_lua_buffer_comment(self):
        # Only Lua's file loader strips this line. Embedded VM buffers reject it.
        self.assertEqual(compact(b"#!/usr/bin/env lua\n  return 1"),
                         b"#!/usr/bin/env lua\nreturn 1")

    def test_end_of_file_does_not_add_a_newline(self):
        for source, expected in ((b"", b""), (b"return 1", b"return 1"),
                                 (b"return 1 --tail", b"return 1"),
                                 (b"return 'x'--[[tail]]", b"return 'x'"),
                                 (b"return [[x]]\t", b"return [[x]]")):
            with self.subTest(source=source):
                self.assertEqual(compact(source), expected)

    def test_unterminated_literals_fail_closed(self):
        for source in (b"'abc", b'"a\\"', b"[=[abc", b"--[=[abc"):
            with self.subTest(source=source), self.assertRaises(ValueError):
                compact(source)

    def test_short_string_newlines_require_an_escape(self):
        for newline in (b"\r", b"\n", b"\r\n", b"\n\r"):
            for quote in (b"'", b'"'):
                with self.subTest(newline=newline, quote=quote):
                    with self.assertRaises(ValueError):
                        compact(quote + b"a" + newline + b"b" + quote)
                    for escape in (b"\\", b"\\z \t"):
                        literal = quote + b"a" + escape + newline + b"b" + quote
                        self.assertEqual(compact(literal), literal)

    def test_nul_cannot_be_hidden_in_a_removed_comment(self):
        for source in (b"return 1 -- \0", b"--[[\0]]return 1", b"return '\0'"):
            with self.subTest(source=source), self.assertRaises(ValueError):
                compact(source)

    def test_cli_default_and_compact_embedding(self):
        source = b"  -- source comment\r\n  return  'a -- b'  \n\r\t"
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            path = root / "source.lua"
            header = root / "resource.h"
            implementation = root / "resource.c"
            path.write_bytes(source)
            command = [sys.executable, str(_PATH), "--source", str(path),
                       "--header", str(header), "--implementation", str(implementation),
                       "--symbol", "fixture"]
            for options, expected in (([], source), (["--compact"], compact(source))):
                subprocess.run(command + options, check=True, capture_output=True)
                generated = implementation.read_text()
                values = generated.split("= {", 1)[1].split("}", 1)[0]
                embedded = bytes(int(value) for value in values.split(",") if value.strip())
                self.assertEqual(embedded, expected)
                subprocess.run(command + options, check=True, capture_output=True)
                self.assertEqual(implementation.read_text(), generated)
            for invalid in (b"\x1bLua", b"--[=[unclosed", b"--\0", b"'a\nb'"):
                path.write_bytes(invalid)
                self.assertNotEqual(subprocess.run(command + ["--compact"],
                                                  capture_output=True).returncode, 0)
            for source, options in ((b"", []), (b"", ["--compact"]),
                                    (b"-- comment only", ["--compact"])):
                path.write_bytes(source)
                subprocess.run(command + options, check=True, capture_output=True)
                self.assertIn("const size_t fixture_size = 0u;", implementation.read_text())
                self.assertIn("  0,", implementation.read_text())


if __name__ == "__main__":
    unittest.main()
