"""AC707N bare-chip compile-only firmware layout."""

load("//tools/bazel:jieli.bzl", "jieli_firmware")

def ac707n_compile_only_firmware(name, graph, **kwargs):
    """Declares firmware using the repository-owned AC707N project."""
    jieli_firmware(
        name = name,
        graph = ["//boards/ac707n_chip/ac707n/layouts/compile_only:layout"] + graph,
        project_makefile = "//boards/ac707n_chip/ac707n/layouts/compile_only:project.mk",
        target = "br35",
        **kwargs
    )
