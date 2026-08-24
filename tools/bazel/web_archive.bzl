"""Run a serve-ready Web archive through a local development server."""

load("@rules_python//python:defs.bzl", "py_binary")

_SERVER = Label("//tools/bazel:web_archive_server.py")
_HOST_LINUX = Label("//tools/bazel/platforms:host_linux_target_linux")
_HOST_MACOS = Label("//tools/bazel/platforms:host_macos_target_macos")
_HOST_WINDOWS = Label("//tools/bazel/platforms:host_windows_target_windows")

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
        srcs = [_SERVER],
        args = [
            "--archive",
            "$(location %s)" % archive,
        ],
        data = [archive],
        legacy_create_init = 0,
        main = _SERVER,
        tags = tags or [],
        target_compatible_with = select({
            _HOST_LINUX: [],
            _HOST_MACOS: [],
            _HOST_WINDOWS: [],
            "//conditions:default": ["@platforms//:incompatible"],
        }),
        visibility = visibility,
    )
