"""AC791N bare-chip compile-only firmware layout."""

load("//tools/bazel:jieli.bzl", "jieli_firmware")

def ac791n_compile_only_firmware(name, **kwargs):
    """Declares firmware using the repository-owned AC791N project."""
    jieli_firmware(
        name = name,
        project_makefile = "//boards/ac791n_chip/ac791n/layouts/compile_only:project.mk",
        target = "wl82",
        **kwargs
    )
