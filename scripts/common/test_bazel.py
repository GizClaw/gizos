"""Repository graph profiles remain explicit and preserve full graph commands."""
from pathlib import Path
import os
import runpy
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bazel import graph_options

ROOT = Path(__file__).resolve().parents[2]


class Executed(Exception):
    """Successful exec replaces the process instead of returning to the script."""


class GraphProfileTests(unittest.TestCase):
    def test_default_and_ci_environment_do_not_select_a_fixture(self):
        for environment in [{}, {"CI": "true"}]:
            with self.subTest(environment=environment), patch.dict(os.environ, environment, clear=True):
                self.assertEqual(graph_options(), [])

    def test_explicit_named_profile(self):
        with patch.dict(os.environ, {"BAZEL_GRAPH_CONFIG": "gizclaw-ci"}, clear=True):
            self.assertEqual(graph_options(), ["--config=gizclaw-ci"])

    def test_reject_options_or_multiple_profiles(self):
        for value in ["--config=other", "a b", "a,b", "x\n--action_env=OTHER"]:
            with self.subTest(value=value), patch.dict(os.environ, {"BAZEL_GRAPH_CONFIG": value}, clear=True):
                with self.assertRaises(ValueError):
                    graph_options()

    def test_build_and_test_entrypoints_keep_the_complete_graph(self):
        for command in ["build", "test"]:
            for enabled in [False, True]:
                environment = {"BAZEL_BIN": "test-bazel", "BAZEL_CONFIG": "macos_arm64"}
                if enabled:
                    environment["BAZEL_GRAPH_CONFIG"] = "gizclaw-ci"
                with self.subTest(command=command, enabled=enabled), patch.dict(os.environ, environment, clear=True), patch("os.execvp", side_effect=Executed) as execute:
                    with self.assertRaises(Executed):
                        runpy.run_path(str(ROOT / "scripts/bazel" / ("bazel-" + command + ".py")), run_name="__main__")
                    execute.assert_called_once()
                    binary, arguments = execute.call_args.args
                    self.assertEqual(binary, "test-bazel")
                    self.assertEqual(arguments[1], command)
                    self.assertEqual(arguments[-1], "//...")
                    self.assertIn("--config=macos_arm64", arguments)
                    self.assertEqual(arguments.count("--config=gizclaw-ci"), int(enabled))


if __name__ == "__main__":
    unittest.main()
