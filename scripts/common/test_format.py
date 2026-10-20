#!/usr/bin/env python3
"""Exercise the format commands in isolated Git repositories, without downloads."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


SCRIPTS = Path(__file__).resolve().parents[1]


class FormatTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="gizos-format-test-")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.repo = self.directory / "repo"
        self.bin = self.directory / "bin"
        self.repo.mkdir()
        self.bin.mkdir()
        self.log = self.directory / "tools.log"
        self.env = dict(os.environ)
        self.env.update(
            PATH=str(self.bin),
            GIT_CONFIG_GLOBAL=os.devnull,
            GIT_CONFIG_NOSYSTEM="1",
            FORMAT_BASE="HEAD",
            XDG_CACHE_HOME=str(self.directory / "cache"),
            CLANG_FORMAT=str(self.bin / "clang-format"),
            STYLUA=str(self.bin / "stylua"),
            BUILDIFIER=str(self.bin / "buildifier"),
            FORMAT_TEST_LOG=str(self.log),
        )
        for command in ("bash", "dirname", "git", "mktemp", "rm", "chmod", "xargs", "uname"):
            (self.bin / command).symlink_to(shutil.which(command))
        self.tool("clang-format", """
if [ "$1" = --version ]; then
  printf '%s\\n' "${FORMAT_TEST_CLANG_VERSION:-clang-format version 22.1.5}"
  exit 0
fi
printf '%s\\n' 'simulated clang-format execution failure' >&2
exit 2
""")
        self.tool("git-clang-format", """
printf '%s\\n' 'git-clang-format' >> "$FORMAT_TEST_LOG"
printf '%s\\n' "$FORMAT_TEST_HELPER_STDOUT"
if [ -n "$FORMAT_TEST_HELPER_STDERR" ]; then
  printf '%s\\n' "$FORMAT_TEST_HELPER_STDERR" >&2
fi
exit "${FORMAT_TEST_HELPER_STATUS:-0}"
""")
        self.tool("stylua", """
printf '%s\\n' "stylua $*" >> "$FORMAT_TEST_LOG"
case " $* " in
  *' --check '*) exit "${FORMAT_TEST_STYLUA_CHECK_STATUS:-0}" ;;
esac
exit 0
""")
        self.tool("buildifier", """
printf '%s\\n' "buildifier $*" >> "$FORMAT_TEST_LOG"
exit "${FORMAT_TEST_BUILDIFIER_STATUS:-0}"
""")
        self.git("init", "-q")
        self.git("config", "user.name", "Formatter Test")
        self.git("config", "user.email", "formatter-test@example.invalid")
        self.git("config", "commit.gpgsign", "false")
        for name, content in (
            ("sample.c", "int value = 0;\n"),
            ("sample.lua", "local value = 0\n"),
            ("BUILD.bazel", "exports_files([])\n"),
        ):
            (self.repo / name).write_text(content)
        self.git("add", ".")
        self.git("commit", "-qm", "baseline")

    def git(self, *args):
        return subprocess.run(
            [str(self.bin / "git"), *args], cwd=self.repo, env=self.env,
            check=True, capture_output=True, text=True,
        )

    def tool(self, name, source):
        path = self.bin / name
        path.write_text("#!/bin/sh\n" + source)
        path.chmod(0o755)

    def change(self, *names):
        for name in names:
            with (self.repo / name).open("a") as output:
                output.write("\n")

    def run_format(self, mode="check", args=(), **env):
        entry = "format.sh" if mode == "fix" else "format-check.sh"
        return subprocess.run(
            [str(SCRIPTS / "config" / entry), *args], cwd=self.repo,
            env={**self.env, **env}, capture_output=True, text=True,
        )

    def assert_failed(self, result, tool="clang-format"):
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(tool, result.stderr)

    def test_no_changes_succeed_without_running_tools(self):
        for mode in ("check", "fix"):
            with self.subTest(mode=mode):
                result = self.run_format(mode)
                self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.log.exists())

    def test_successful_no_change_check(self):
        self.change("sample.c")
        result = self.run_format(FORMAT_TEST_HELPER_STDOUT="clang-format did not modify any files")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_formatting_diff_fails_check(self):
        self.change("sample.c")
        self.assert_failed(self.run_format(
            FORMAT_TEST_HELPER_STATUS="1", FORMAT_TEST_HELPER_STDOUT="diff --git a/sample.c b/sample.c",
        ))

    def test_successful_rewrite_is_not_a_tool_error(self):
        self.change("sample.c")
        result = self.run_format(
            "fix", FORMAT_TEST_HELPER_STATUS="1",
            FORMAT_TEST_HELPER_STDOUT="changed files:\n    sample.c",
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_helper_errors_fail_check_and_fix_and_other_tools_still_run(self):
        self.change("sample.c", "sample.lua", "BUILD.bazel")
        for mode in ("check", "fix"):
            for output in ("", "no modified files to format"):
                with self.subTest(mode=mode, output=output):
                    self.log.unlink(missing_ok=True)
                    result = self.run_format(
                        mode, FORMAT_TEST_HELPER_STATUS="2",
                        FORMAT_TEST_HELPER_STDOUT=output, FORMAT_TEST_HELPER_STDERR="simulated formatter failure",
                    )
                    self.assert_failed(result)
                    self.assertIn("simulated formatter failure", result.stderr)
                    log = self.log.read_text()
                    self.assertIn("stylua ", log)
                    self.assertIn("buildifier ", log)

    def test_unexpected_exit_one_is_not_a_successful_fix(self):
        self.change("sample.c")
        self.assert_failed(self.run_format("fix", FORMAT_TEST_HELPER_STATUS="1"))

    def test_missing_helper_fails_check_and_fix(self):
        self.change("sample.c")
        (self.bin / "git-clang-format").unlink()
        for mode in ("check", "fix"):
            with self.subTest(mode=mode):
                self.assert_failed(self.run_format(mode))

    def test_unavailable_binary_does_not_skip_other_tools(self):
        self.change("sample.c", "sample.lua", "BUILD.bazel")
        for mode in ("check", "fix"):
            with self.subTest(mode=mode):
                result = self.run_format(mode, CLANG_FORMAT=str(self.bin / "missing-formatter"))
                self.assert_failed(result)
                self.assertIn("buildifier ", self.log.read_text())

    def test_explicit_binary_must_match_exact_pinned_version(self):
        self.change("sample.c")
        for version in ("21.1.8", "22.1.50"):
            with self.subTest(version=version):
                result = self.run_format(FORMAT_TEST_CLANG_VERSION=f"clang-format version {version}")
                self.assert_failed(result)
                self.assertIn("22.1.5 is required", result.stderr)

    def test_invalid_base_fails(self):
        self.change("sample.c")
        for mode in ("check", "fix"):
            with self.subTest(mode=mode):
                result = self.run_format(mode, FORMAT_BASE="missing-base")
                self.assertNotEqual(result.returncode, 0)

    def test_all_scope_reports_binary_failure_and_continues(self):
        for mode in ("check", "fix"):
            with self.subTest(mode=mode):
                result = self.run_format(mode, args=("--all",))
                self.assert_failed(result)
                self.assertIn("simulated clang-format execution failure", result.stderr)
                self.assertIn("stylua ", self.log.read_text())
                self.assertIn("buildifier ", self.log.read_text())

    def test_stylua_must_stabilize_before_fix_succeeds(self):
        self.change("sample.lua", "BUILD.bazel")
        result = self.run_format("fix", FORMAT_TEST_STYLUA_CHECK_STATUS="1")
        self.assert_failed(result, "stylua")
        log = self.log.read_text()
        self.assertEqual(log.count("--check"), 5)
        self.assertIn("buildifier ", log)

    def test_failed_tool_download_does_not_skip_remaining_tools(self):
        self.change("sample.lua", "BUILD.bazel")
        self.tool("curl", "exit 22\n")
        result = self.run_format(STYLUA="")
        self.assert_failed(result, "stylua")
        self.assertIn("buildifier ", self.log.read_text())


if __name__ == "__main__":
    unittest.main()
