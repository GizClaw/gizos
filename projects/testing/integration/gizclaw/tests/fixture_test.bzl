"""Exercise required fixture analysis and native-component propagation."""

load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts")
load("@bazel_skylib//rules:common_settings.bzl", "string_flag")
load("//projects/testing/integration/gizclaw:fixture.bzl", "bk_gizclaw_fixture_check")
load("//tools/bazel/firmware:native_component.bzl", "firmware_native_component")

_FIELDS = ["key", "profile", "value", "endpoint", "token", "device_api_url", "audio_url", "root_ca_hex"]
_CANARY = "fixture-token-canary-never-include-in-diagnostics"

def _failure_impl(ctx):
    env = analysistest.begin(ctx)
    asserts.expect_failure(env, "BK GizClaw E2E configuration is incomplete")
    for name in ctx.attr.expected_missing:
        asserts.expect_failure(env, name)
    failures = analysistest.target_under_test(env)[AnalysisFailureInfo].causes.to_list()
    for cause in failures:
        asserts.false(env, _CANARY in cause.message, "Credential values must not appear in analysis errors")
    return analysistest.end(env)

_failure_test = analysistest.make(
    _failure_impl,
    expect_failure = True,
    attrs = {"expected_missing": attr.string_list()},
)

def _success_impl(ctx):
    env = analysistest.begin(ctx)
    files = analysistest.target_under_test(env)[DefaultInfo].files.to_list()
    asserts.true(env, ctx.attr.expected_marker in [file.basename for file in files])
    for action in analysistest.target_actions(env):
        if hasattr(action, "content") and action.content != None:
            asserts.false(env, _CANARY in action.content, "The validation marker must not contain credentials")
    return analysistest.end(env)

_success_test = analysistest.make(
    _success_impl,
    attrs = {"expected_marker": attr.string(mandatory = True)},
)

def fixture_test_suite(name, compatibility):
    """Use analysis-only inputs, independent of private or ambient CLI values."""
    string_flag(
        name = name + "_empty",
        testonly = True,
        build_setting_default = "",
    )
    string_flag(
        name = name + "_configured",
        testonly = True,
        build_setting_default = "configured-analysis-fixture",
    )
    string_flag(
        name = name + "_token",
        testonly = True,
        build_setting_default = _CANARY,
    )
    configured = {field: ":" + name + "_configured" for field in _FIELDS}
    configured["token"] = ":" + name + "_token"
    tests = []
    for suffix, absent in [
        ("all_missing", _FIELDS),
        ("trust_missing", ["root_ca_hex"]),
        ("token_missing", ["token"]),
        ("endpoint_missing", ["endpoint"]),
    ]:
        target = name + "_" + suffix
        bk_gizclaw_fixture_check(
            name = target,
            testonly = True,
            tags = ["manual"],
            **(configured | {field: ":" + name + "_empty" for field in absent})
        )
        _failure_test(
            name = target + "_test",
            target_under_test = ":" + target,
            expected_missing = absent,
            target_compatible_with = compatibility,
        )
        tests.append(":" + target + "_test")
    valid = name + "_complete"
    bk_gizclaw_fixture_check(
        name = valid,
        testonly = True,
        tags = ["manual"],
        **configured
    )
    _success_test(
        name = valid + "_test",
        target_under_test = ":" + valid,
        expected_marker = valid + ".checked",
        target_compatible_with = compatibility,
    )
    tests.append(":" + valid + "_test")
    for suffix, guard, success in [
        ("native_complete", valid, True),
        ("native_missing", name + "_trust_missing", False),
    ]:
        consumer = name + "_" + suffix
        firmware_native_component(
            name = consumer,
            testonly = True,
            component_name = "ap_fixture_test",
            execution_unit = "ap",
            data = [":" + guard],
            tags = ["manual"],
        )
        if success:
            _success_test(
                name = consumer + "_test",
                target_under_test = ":" + consumer,
                expected_marker = valid + ".checked",
                target_compatible_with = compatibility,
            )
        else:
            _failure_test(
                name = consumer + "_test",
                target_under_test = ":" + consumer,
                expected_missing = ["root_ca_hex"],
                target_compatible_with = compatibility,
            )
        tests.append(":" + consumer + "_test")
    native.test_suite(name = name, tests = tests)
