"""SDK-free lifetime regression for a board-owned BK audio implementation."""

load("@rules_cc//cc:defs.bzl", "cc_test")
load("//tools/bazel:cc_options.bzl", "H2_C11_OPTS", "H2_WARNING_COPTS")

def bk_audio_reader_test(name, source):
    native.genrule(
        name = name + "_source",
        srcs = [source],
        outs = ["h2_audio_reader_stop.inc"],
        cmd = "$(location %s) $(location %s) $@" % (
            Label("//native_component_src/bk7258/ap/h2_pal_core:audio_reader_source"),
            source,
        ),
        tools = [Label("//native_component_src/bk7258/ap/h2_pal_core:audio_reader_source")],
    )
    cc_test(
        name = name,
        srcs = [
            Label("//native_component_src/bk7258/ap/h2_pal_core:tests/audio_reader_probe.c"),
            ":" + name + "_source",
        ],
        conlyopts = H2_C11_OPTS,
        copts = H2_WARNING_COPTS,
        includes = ["."],
        target_compatible_with = select({
            Label("//tools/bazel/platforms:host_linux_target_linux"): [],
            Label("//tools/bazel/platforms:host_macos_target_macos"): [],
            "//conditions:default": ["@platforms//:incompatible"],
        }),
        deps = [Label("//native_component_src/bk7258/ap/h2_pal_core:audio_reader_lifetime")],
    )
