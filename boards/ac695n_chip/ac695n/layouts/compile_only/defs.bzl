"""AC695N bare-chip compile-only firmware layout."""

load("//tools/bazel:jieli.bzl", "jieli_firmware")

def ac695n_compile_only_firmware(name, **kwargs):
    """Declares firmware using the AC695N compile-only SDK layout."""
    jieli_firmware(
        name = name,
        sdk_project = ".",
        target = "br23",
        **kwargs
    )
