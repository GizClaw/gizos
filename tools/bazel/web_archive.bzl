"""Run a serve-ready Web archive through a local development server."""

load("@rules_python//python:defs.bzl", "py_binary")

def web_archive_serve(name, archive, visibility = None, tags = None):
    """Creates a `bazel run` target that serves one tar archive.

    Args:
      name: Executable target name.
      archive: Label of the serve-ready tar archive.
      visibility: Optional target visibility.
      tags: Optional Bazel tags.
    """
    py_binary(
        name = name,
        srcs = ["//tools/bazel:web_archive_server.py"],
        args = [
            "--archive",
            "$(location %s)" % archive,
        ],
        data = [archive],
        legacy_create_init = 0,
        main = "//tools/bazel:web_archive_server.py",
        tags = tags or [],
        target_compatible_with = select({
            "//tools/bazel/platforms:host_linux_target_linux": [],
            "//tools/bazel/platforms:host_macos_target_macos": [],
            "//tools/bazel/platforms:host_windows_target_windows": [],
            "//conditions:default": ["@platforms//:incompatible"],
        }),
        visibility = visibility,
    )
