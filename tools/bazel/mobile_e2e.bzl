"""Direct Python PAL mobile E2E test declaration (no shell trampoline)."""

load("@rules_python//python:defs.bzl", "py_test")

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

def mobile_e2e_test(name, runner, args, data, env_inherit, target_compatible_with, deps = [], timeout = "moderate"):
    py_test(
        name = name,
        srcs = [runner],
        main = runner,
        args = args,
        data = data,
        deps = [Label("//tools/bazel:mobile_e2e_runtime")] + deps,
        env_inherit = env_inherit,
        legacy_create_init = False,
        tags = ["manual"],
        local = True,
        timeout = timeout,
        target_compatible_with = target_compatible_with,
    )
