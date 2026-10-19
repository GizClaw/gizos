"""Direct Python PAL mobile E2E test declaration (no shell trampoline)."""

load("@rules_python//python:defs.bzl", "py_library", "py_test")
load("@rules_python//python:py_info.bzl", "PyInfo")
load("//tools/bazel/platforms:compatibility.bzl", "ANDROID_ARM64_PACKAGE_ARTIFACT_COMPATIBILITY", "HOST_OR_MOBILE_TOOL_COMPATIBILITY", "IOS_SIM_ARM64_ARTIFACT_COMPATIBILITY")

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

def _suite_impl(ctx):
    config = json.decode(ctx.attr.config)
    config["registry"] = ctx.file.registry.short_path
    config["hook"] = ctx.file.hook.short_path if ctx.file.hook else None
    config["fixtures"] = {}
    for target, name in ctx.attr.fixtures.items():
        files = target[DefaultInfo].files.to_list()
        if len(files) != 1:
            fail("fixture {} must produce one file".format(name))
        config["fixtures"][name] = files[0].short_path
    output = ctx.actions.declare_file(ctx.label.name + ".json")
    ctx.actions.write(output, json.encode(config) + "\n")
    return [
        DefaultInfo(files = depset([output]), runfiles = ctx.runfiles(files = [output]).merge(ctx.attr.python[DefaultInfo].default_runfiles)),
        ctx.attr.python[PyInfo],
    ]

_suite = rule(
    implementation = _suite_impl,
    attrs = {
        "config": attr.string(mandatory = True),
        "registry": attr.label(allow_single_file = True, mandatory = True),
        "hook": attr.label(allow_single_file = [".py"]),
        "fixtures": attr.label_keyed_string_dict(allow_files = True),
        "python": attr.label(providers = [PyInfo], mandatory = True),
    },
)

def mobile_e2e_suite(name, package, report, registry, registry_pattern, expected, case_result = "rc", case_count = 0, optional_cases = [], resource_balance = False, timeout_seconds = 90, ios_sdk = {}, android_sdk = {}, ios_permissions = [], android_permissions = [], capture_png = False, android_log = None, prefix = None, output_default = None, plain_platform = False, options = {}, hook = None, fixtures = {}, deps = []):
    """Declare one suite's data contract and optional imperative hook once."""
    python = name + "_python"
    py_library(
        name = python,
        srcs = [hook] if hook else [],
        data = [registry] + fixtures.values(),
        deps = deps,
        target_compatible_with = HOST_OR_MOBILE_TOOL_COMPATIBILITY,
        visibility = ["//visibility:private"],
    )
    config = {
        "package": package,
        "report": report,
        "registry_pattern": registry_pattern,
        "expected": expected,
        "case_result": case_result,
        "case_count": case_count,
        "resource_balance": resource_balance,
        "timeout": timeout_seconds,
        "ios_sdk": ios_sdk,
        "android_sdk": android_sdk,
        "permissions": {"ios": ios_permissions, "android": android_permissions},
        "capture_png": capture_png,
        "android_log": android_log,
        "prefix": prefix,
        "output_default": output_default,
        "plain_platform": plain_platform,
        "options": options,
    }
    if optional_cases:
        config["optional_cases"] = optional_cases
    _suite(
        name = name,
        config = json.encode(config),
        registry = registry,
        hook = hook,
        fixtures = {label: key for key, label in fixtures.items()},
        python = ":" + python,
        target_compatible_with = HOST_OR_MOBILE_TOOL_COMPATIBILITY,
    )

def mobile_e2e_test(name, platform, suite, app, sdk, env_inherit = [], timeout = "moderate", external = False):
    """Bind a declared suite to a packaged consumer of the same Python main."""
    if platform not in ["ios", "android"]:
        fail("mobile E2E platform must be ios or android")
    runner = Label("//tools/bazel:mobile_e2e.py")
    py_test(
        name = name,
        srcs = [runner],
        main = runner,
        args = ["--suite", "$(rootpath {})".format(suite), platform] + [
            "$(rootpath {})".format(label)
            for label in [app, sdk]
        ],
        data = [app, sdk, suite],
        deps = [suite],
        env_inherit = (["H2_IOS_SIMULATOR_UDID", "DEVELOPER_DIR"] if platform == "ios" else ["H2_ANDROID_SERIAL", "ANDROID_HOME", "ANDROID_NDK_HOME"]) + env_inherit,
        legacy_create_init = False,
        tags = ["manual"] + (["external"] if external else []),
        local = True,
        timeout = timeout,
        target_compatible_with = IOS_SIM_ARM64_ARTIFACT_COMPATIBILITY if platform == "ios" else ANDROID_ARM64_PACKAGE_ARTIFACT_COMPATIBILITY,
    )
