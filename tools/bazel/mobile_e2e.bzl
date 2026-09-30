"""Direct Python PAL mobile E2E test declaration (no shell trampoline)."""

load("@rules_python//python:defs.bzl", "py_test")
load("//tools/bazel/platforms:compatibility.bzl", "ANDROID_ARM64_PACKAGE_ARTIFACT_COMPATIBILITY", "IOS_SIM_ARM64_ARTIFACT_COMPATIBILITY")

def _host_python_runtime_impl(ctx):
    return [ctx.attr.runtime[platform_common.ToolchainInfo]]

# The consumer stays in its iOS/Android configuration, while the interpreter
# runs beside simctl/adb. Resolve the existing hermetic runtime in exec config.
host_python_runtime = rule(
    implementation = _host_python_runtime_impl,
    attrs = {
        "runtime": attr.label(
            default = Label("@python_3_11//:python_runtimes"),
            cfg = "exec",
            providers = [platform_common.ToolchainInfo],
        ),
    },
)

def mobile_e2e_test(name, platform, suite, app, sdk, registry, args = [], data = [], deps = [], env_inherit = [], timeout = "moderate"):
    """Declare artifacts once; all suites execute the same Python main."""
    if platform not in ["ios", "android"]:
        fail("mobile E2E platform must be ios or android")
    runner = Label("//tools/bazel:mobile_e2e.py")
    py_test(
        name = name,
        srcs = [runner, suite],
        main = runner,
        args = ["--suite", "$(rootpath {})".format(suite), platform] + [
            "$(rootpath {})".format(label)
            for label in [app, sdk, registry]
        ] + args,
        data = [app, sdk, registry] + data,
        deps = deps,
        env_inherit = (["H2_IOS_SIMULATOR_UDID", "DEVELOPER_DIR"] if platform == "ios" else ["H2_ANDROID_SERIAL", "ANDROID_HOME", "ANDROID_NDK_HOME"]) + env_inherit,
        legacy_create_init = False,
        tags = ["manual"],
        local = True,
        timeout = timeout,
        target_compatible_with = IOS_SIM_ARM64_ARTIFACT_COMPATIBILITY if platform == "ios" else ANDROID_ARM64_PACKAGE_ARTIFACT_COMPATIBILITY,
    )
