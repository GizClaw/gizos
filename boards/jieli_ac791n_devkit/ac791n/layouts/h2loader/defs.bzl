"""Compatibility wrapper for the AC791N board's one H2Loader layout."""

load("//projects/h2loader/tools/bazel:h2loader_firmware.bzl", "h2loader_jieli_firmware")

def jieli_ac791n_devkit_h2loader_firmware(name, graph, task_policy, **kwargs):
    h2loader_jieli_firmware(
        name = name,
        board = "jieli_ac791n_devkit",
        graph = graph,
        target = "wl82",
        task_policy = task_policy,
        **kwargs
    )
