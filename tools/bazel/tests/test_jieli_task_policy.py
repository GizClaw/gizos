"""Unit checks of the generator's row parser, with Starlark string adapter."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[3]


class TaskPolicyTest(unittest.TestCase):
    def setUp(self):
        source = (ROOT / "tools/bazel/jieli_task_policy.bzl").read_text()
        parser = source[source.index("def _row("):source.index("def _source_impl(")]
        def fail(message):
            raise ValueError(message)
        namespace = {"fail": fail}
        # Python strings are iterable; Starlark explicitly exposes elems().
        exec(parser.replace("name.elems()", "name"), namespace)
        self.parse = namespace["_row"]

    def test_preserves_sdk_units_and_affinity(self):
        self.assertEqual(self.parse("#C0$mp4-player/audio 10 2048 128"),
                         ("#C0$mp4-player/audio", '{"#C0$mp4-player/audio", 10u, 2048u, 128u, 0}'))

    def test_rejects_unsafe_names_and_overflow(self):
        for row in ("task 256 1 0", "task 1 0 0", "task 1 4294967296 0",
                    "task 1 1 65536", "task -1 1 0", "task 1 1",
                    "bad\nname 1 1 0", "bad*name 1 1 0", "$h2anon/abc 1 1 0",
                    "#C0 1 1 0", "x" * 32 + " 1 1 0"):
            with self.subTest(row=row), self.assertRaises(ValueError):
                self.parse(row)

    def test_boundary_values(self):
        self.parse("x" * 31 + " 255 4294967295 65535")
        self.parse("#C0" + "x" * 31 + " 0 1 0")
