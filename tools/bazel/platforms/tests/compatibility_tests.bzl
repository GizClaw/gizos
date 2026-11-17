"""Check host-tool compatibility selects against real platform configurations."""

load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts")

_CompatibilityInfo = provider(fields = ["host", "posix"])

def _fixture_impl(ctx):
    return [_CompatibilityInfo(
        host = not ctx.attr.host_constraints,
        posix = not ctx.attr.posix_constraints,
    )]

compatibility_fixture = rule(
    implementation = _fixture_impl,
    attrs = {
        "host_constraints": attr.label_list(),
        "posix_constraints": attr.label_list(),
    },
)

def _compatibility_test_impl(ctx):
    env = analysistest.begin(ctx)
    selected = analysistest.target_under_test(env)[_CompatibilityInfo]
    asserts.equals(env, ctx.attr.expected_host, selected.host)
    asserts.equals(env, ctx.attr.expected_posix, selected.posix)
    return analysistest.end(env)

def _configured_test(platform):
    return analysistest.make(
        _compatibility_test_impl,
        attrs = {
            "expected_host": attr.bool(),
            "expected_posix": attr.bool(),
        },
        config_settings = {
            "//command_line_option:platforms": str(Label("//tools/bazel/platforms:" + platform)),
        },
    )

# h2_host_os is inherited from the caller. Bazel does not permit transitions
# on --define; run this matrix with the Linux, macOS and Windows host flags.
linux_arm64_compatibility_test = _configured_test("linux_arm64")
linux_x86_64_compatibility_test = _configured_test("linux_x86_64")
macos_arm64_compatibility_test = _configured_test("macos_arm64")
windows_x86_64_compatibility_test = _configured_test("windows_x86_64")
firmware_compatibility_test = _configured_test("bk7258")
