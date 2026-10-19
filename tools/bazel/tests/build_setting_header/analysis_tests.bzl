"""Verify build_setting_header rejects non-canonical or out-of-range overrides."""

load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts")

def _failure_impl(ctx):
    env = analysistest.begin(ctx)
    asserts.expect_failure(env, ctx.attr.expected_message)
    return analysistest.end(env)

header_failure_test = analysistest.make(
    _failure_impl,
    expect_failure = True,
    attrs = {"expected_message": attr.string(mandatory = True)},
)

def _success_impl(ctx):
    env = analysistest.begin(ctx)
    files = analysistest.target_under_test(env)[DefaultInfo].files.to_list()
    asserts.equals(env, 1, len(files))
    asserts.equals(env, "h2_fixture_cycles.h", files[0].basename)
    return analysistest.end(env)

header_success_test = analysistest.make(_success_impl)
