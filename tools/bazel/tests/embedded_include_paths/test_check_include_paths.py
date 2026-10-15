"""The include path checker rejects output-base-specific header paths."""

import pathlib
import subprocess
import sys
import tempfile
import unittest

from tools.bazel.tests.embedded_include_paths import check_include_paths

_BUILTIN = "external/+local_embedded_cc_toolchains+gizos_bk7258_cc_toolchain/inputs/compiler/include/0"
_OUTPUT_BASE_HEADER = (
    "/Volumes/cache/output-user-root/dcd2aa431a7b071416397c3eb0737ebe/external/"
    "gizos++bk_repositories+gizos_bk_arm_toolchain/toolchain/arm-none-eabi/include/stdlib.h"
)


class DependencyPathsTest(unittest.TestCase):
    def test_parses_continuations_and_escaped_spaces(self):
        text = "out/probe.o: probe.c \\\n dir\\ with\\ space/a.h \\\n  b.h\n"

        self.assertEqual(
            check_include_paths.dependency_paths(text),
            ["probe.c", "dir with space/a.h", "b.h"],
        )


class CheckTest(unittest.TestCase):
    def test_accepts_relative_system_headers(self):
        text = f"probe.o: probe.c {_BUILTIN}/stddef.h\n"

        self.assertEqual(check_include_paths.check([_BUILTIN], {"probe.d": text}), [])

    def test_rejects_absolute_dependency_from_another_output_base(self):
        text = f"probe.o: probe.c {_BUILTIN}/stddef.h {_OUTPUT_BASE_HEADER}\n"

        errors = check_include_paths.check([_BUILTIN], {"probe.d": text})

        self.assertEqual(len(errors), 1)
        self.assertIn(_OUTPUT_BASE_HEADER, errors[0])

    def test_rejects_absolute_builtin_include_directory(self):
        directory = "/Volumes/cache/output-user-root/a6f78643/external/toolchain/include"
        text = f"probe.o: probe.c {_BUILTIN}/stddef.h\n"

        errors = check_include_paths.check([_BUILTIN, directory], {"probe.d": text})

        self.assertEqual(errors, [f"builtin include directory is absolute: {directory}"])

    def test_rejects_dependency_file_without_system_headers(self):
        errors = check_include_paths.check([_BUILTIN], {"probe.d": "probe.o: probe.c local.h\n"})

        self.assertEqual(errors, ["probe.d: no dependency lies under a builtin include directory"])

    def test_rejects_missing_builtin_include_directories(self):
        errors = check_include_paths.check([], {"probe.d": f"probe.o: probe.c {_BUILTIN}/stddef.h\n"})

        self.assertIn("the toolchain declares no builtin include directories", errors)


class CommandLineTest(unittest.TestCase):
    def _run(self, root: pathlib.Path, dependency_text: str) -> subprocess.CompletedProcess:
        dependency_file = root / "probe.d"
        dependency_file.write_text(dependency_text, encoding="utf-8")
        return subprocess.run(
            [
                sys.executable,
                str(pathlib.Path(check_include_paths.__file__).resolve()),
                "--builtin-include-directory",
                _BUILTIN,
                "--dependency-file",
                str(dependency_file),
                "--output",
                str(root / "report.txt"),
            ],
            check=False,
            capture_output=True,
            text=True,
        )

    def test_success_writes_report(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)

            result = self._run(root, f"probe.o: probe.c {_BUILTIN}/stddef.h\n")

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(f"builtin {_BUILTIN}", (root / "report.txt").read_text(encoding="utf-8"))

    def test_failure_exits_nonzero_without_report(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)

            result = self._run(root, f"probe.o: probe.c {_OUTPUT_BASE_HEADER}\n")

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("depend on the Bazel output base", result.stderr)
            self.assertFalse((root / "report.txt").exists())


if __name__ == "__main__":
    unittest.main()
