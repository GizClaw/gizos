"""Check pure version declarations, typed inputs, and firmware composition."""

load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts")
load("//tools/bazel:board_version.bzl", "BoardVersionInfo", "board_version")
load("//tools/bazel:firmware.bzl", "FirmwareVersionInfo", "firmware_version")

def _success_impl(ctx):
    env = analysistest.begin(ctx)
    target = analysistest.target_under_test(env)
    asserts.equals(env, ctx.attr.expected_version, target[FirmwareVersionInfo].value)
    asserts.false(env, BoardVersionInfo in target)
    return analysistest.end(env)

version_success_test = analysistest.make(
    _success_impl,
    attrs = {"expected_version": attr.string(mandatory = True)},
)

def _board_success_impl(ctx):
    env = analysistest.begin(ctx)
    target = analysistest.target_under_test(env)
    asserts.equals(env, ctx.attr.expected_build, target[BoardVersionInfo].build)
    asserts.false(env, FirmwareVersionInfo in target)
    asserts.equals(env, [], target[DefaultInfo].files.to_list())
    return analysistest.end(env)

board_success_test = analysistest.make(
    _board_success_impl,
    attrs = {"expected_build": attr.int(mandatory = True)},
)

def _failure_impl(ctx):
    env = analysistest.begin(ctx)
    asserts.expect_failure(env, ctx.attr.expected_message)
    return analysistest.end(env)

version_failure_test = analysistest.make(
    _failure_impl,
    expect_failure = True,
    attrs = {"expected_message": attr.string(mandatory = True)},
)

def firmware_success_case(name, attributes, expected_version, compatibility):
    firmware_version(name = name, testonly = True, **attributes)
    version_success_test(
        name = name + "_test",
        expected_version = expected_version,
        target_compatible_with = compatibility,
        target_under_test = ":" + name,
    )

def firmware_failure_case(name, attributes, expected_message, compatibility):
    firmware_version(name = name, testonly = True, tags = ["manual"], **attributes)
    version_failure_test(
        name = name + "_test",
        expected_message = expected_message,
        target_compatible_with = compatibility,
        target_under_test = ":" + name,
    )

def board_failure_case(name, build, expected_message, compatibility):
    board_version(name = name, testonly = True, tags = ["manual"], build = build)
    version_failure_test(
        name = name + "_test",
        expected_message = expected_message,
        target_compatible_with = compatibility,
        target_under_test = ":" + name,
    )
