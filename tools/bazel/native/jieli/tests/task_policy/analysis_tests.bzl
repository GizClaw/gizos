"""Verify graph mismatches fail through the macro's native component."""

load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts")

def _policy_failure_impl(ctx):
    env = analysistest.begin(ctx)
    asserts.expect_failure(env, ctx.attr.expected_message)
    asserts.expect_failure(env, ctx.attr.expected_task)
    return analysistest.end(env)

policy_failure_test = analysistest.make(
    _policy_failure_impl,
    expect_failure = True,
    attrs = {
        "expected_message": attr.string(mandatory = True),
        "expected_task": attr.string(mandatory = True),
    },
)
