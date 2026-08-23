"""AC791N bare-chip compile-only firmware layout."""

load("//tools/bazel:jieli.bzl", "jieli_firmware")

def ac791n_compile_only_firmware(name, **kwargs):
    """Declares firmware using the AC791N compile-only SDK layout."""
    jieli_firmware(
        name = name,
        sdk_project = "apps/demo/demo_hello/board/wl82",
        target = "wl82",
        **kwargs
    )
