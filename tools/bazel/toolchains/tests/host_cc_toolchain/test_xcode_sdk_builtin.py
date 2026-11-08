"""Check that the macOS host C/C++ toolchain declares the Xcode SDK root as builtin.

Xcode 27 clang records `<SDK>/SDKSettings.json` in every `-MD` dependency file.
Bazel rejects that absolute path unless it lies under one of the toolchain's
`cxx_builtin_include_directories`, so a toolchain that only declares the SDK's
`usr/include` and framework directories fails every host compile.
"""

from __future__ import annotations

import os
import posixpath
import subprocess
import unittest


def is_covered(path: str, directories: list[str]) -> bool:
    """Returns whether Bazel's header inclusion check accepts `path`."""
    for directory in directories:
        if not posixpath.isabs(directory):
            continue
        root = posixpath.normpath(directory)
        if root == "/" or path == root or path.startswith(root + "/"):
            return True
    return False


def uncovered_sdk_files(sdk_path: str, real_sdk_path: str, directories: list[str]) -> list[str]:
    """Returns every spelling of `SDKSettings.json` that the directories do not cover."""
    candidates = sorted({
        posixpath.join(posixpath.normpath(sdk_path), "SDKSettings.json"),
        posixpath.join(posixpath.normpath(real_sdk_path), "SDKSettings.json"),
    })
    return [candidate for candidate in candidates if not is_covered(candidate, directories)]


class CoverageTest(unittest.TestCase):
    SDK = "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX27.0.sdk"
    REAL_SDK = "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk"

    def test_sdk_subdirectories_do_not_cover_the_sdk_root(self):
        # rules_cc's generic local_config_cc declares only these SDK directories.
        directories = [
            self.REAL_SDK + "/usr/include",
            self.REAL_SDK + "/System/Library/Frameworks",
            "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk",
        ]
        self.assertEqual(
            uncovered_sdk_files(self.SDK, self.REAL_SDK, directories),
            [self.REAL_SDK + "/SDKSettings.json", self.SDK + "/SDKSettings.json"],
        )

    def test_applications_covers_both_sdk_spellings(self):
        # apple_support's local_config_apple_cc declares these directories.
        directories = ["/Applications/", "/Library/"]
        self.assertEqual(uncovered_sdk_files(self.SDK, self.REAL_SDK, directories), [])

    def test_matching_is_by_path_component(self):
        self.assertFalse(is_covered("/ApplicationsExtra/SDKSettings.json", ["/Applications"]))
        self.assertTrue(is_covered("/Applications/SDKSettings.json", ["/Applications"]))

    def test_relative_directories_never_cover_absolute_paths(self):
        self.assertFalse(is_covered("/Applications/SDKSettings.json", ["Applications", "external/sdk"]))


class ResolvedToolchainTest(unittest.TestCase):
    def test_resolved_toolchain_covers_selected_xcode_sdk(self):
        listing = os.environ["H2_CC_BUILTIN_INCLUDE_DIRECTORIES"]
        with open(listing, encoding="utf-8") as stream:
            directories = [line for line in stream.read().splitlines() if line]
        self.assertTrue(directories, "the resolved C/C++ toolchain declares no builtin include directories")

        sdk_path = subprocess.run(
            ["/usr/bin/xcrun", "--sdk", "macosx", "--show-sdk-path"],
            capture_output=True,
            check=True,
            text=True,
        ).stdout.strip()
        real_sdk_path = os.path.realpath(sdk_path)
        self.assertTrue(
            os.path.isfile(posixpath.join(real_sdk_path, "SDKSettings.json")),
            f"{sdk_path} has no SDKSettings.json",
        )
        self.assertEqual(
            uncovered_sdk_files(sdk_path, real_sdk_path, directories),
            [],
            "the resolved C/C++ toolchain does not declare the Xcode SDK root as builtin; "
            f"builtin include directories: {directories}",
        )


if __name__ == "__main__":
    unittest.main()
