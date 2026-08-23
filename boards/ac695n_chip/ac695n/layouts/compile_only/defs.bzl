"""AC695N bare-chip compile-only firmware layout."""

load("//tools/bazel:jieli.bzl", "jieli_firmware")

def ac695n_compile_only_firmware(name, graph, **kwargs):
    """Declares firmware using the repository-owned AC695N project."""
    jieli_firmware(
        name = name,
        graph = ["//boards/ac695n_chip/ac695n/layouts/compile_only:layout"] + graph,
        project_makefile = "//boards/ac695n_chip/ac695n/layouts/compile_only:project.mk",
        target = "br23",
        **kwargs
    )
