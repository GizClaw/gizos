"""Compile actual Loader startup functions against a minimal SDK fixture."""

load("@rules_cc//cc:defs.bzl", "cc_test")
load("//tools/bazel/platforms:compatibility.bzl", "HOST_TOOL_COMPATIBILITY")

def loader_usb_fixture(name, generator, source):
    native.genrule(
        name = name + "_source",
        srcs = [source],
        outs = [name + ".c"],
        tools = [generator],
        cmd = "$(execpath %s) $(location %s) $@" % (generator, source),
    )
    cc_test(name = name, srcs = [":" + name + "_source"], target_compatible_with = HOST_TOOL_COMPATIBILITY)
