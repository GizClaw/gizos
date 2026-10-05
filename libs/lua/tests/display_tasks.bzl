"""Exercise real Lua dependency closures and the unchanged task-policy audit."""

load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts")
load("//tools/bazel:target_task_policy_codegen.bzl", "task_policy_audit")
load("//tools/bazel:tasks.bzl", "H2TasksInfo", "h2_tasks")

def _inventory_impl(ctx):
    env = analysistest.begin(ctx)
    tasks = analysistest.target_under_test(env)[H2TasksInfo].tasks.to_list()
    asserts.equals(env, ctx.attr.expected, sorted([task.name for task in tasks]))
    return analysistest.end(env)

_inventory_test = analysistest.make(
    _inventory_impl,
    attrs = {"expected": attr.string_list()},
)

def _audit_impl(ctx):
    env = analysistest.begin(ctx)
    return analysistest.end(env)

_audit_test = analysistest.make(_audit_impl)

def _missing_impl(ctx):
    env = analysistest.begin(ctx)
    asserts.expect_failure(env, "$lua/display")
    asserts.expect_failure(env, "display_worker_tasks")
    return analysistest.end(env)

_missing_test = analysistest.make(_missing_impl, expect_failure = True)

def display_task_tests():
    """Default consumers need no Display policy; opted-in consumers still do."""
    compatible = select({
        Label("//tools/bazel/platforms:host_linux_target_linux"): [],
        Label("//tools/bazel/platforms:host_macos_target_macos"): [],
        "//conditions:default": ["@platforms//:incompatible"],
    })
    for name, library, tasks in [
        ("inline", ":lua_runtime", ["$lua/worker", "$runtime/input", "$runtime/nfc"]),
        ("worker", ":lua_display_worker", ["$lua/display", "$lua/worker", "$runtime/input", "$runtime/nfc"]),
    ]:
        h2_tasks(
            name = "display_" + name + "_inventory",
            testonly = True,
            tasks = [],
            deps = [library],
        )
        _inventory_test(
            name = "display_" + name + "_inventory_test",
            target_under_test = ":display_" + name + "_inventory",
            expected = tasks,
            target_compatible_with = compatible,
        )
        task_policy_audit(
            name = "display_" + name + "_audit",
            testonly = True,
            graph = [library],
            default_tasks = tasks,
            policies_json = "{}",
            policy_label = "display_" + name + "_policy",
        )
        _audit_test(
            name = "display_" + name + "_policy_test",
            target_under_test = ":display_" + name + "_audit",
            target_compatible_with = compatible,
        )
    task_policy_audit(
        name = "display_worker_missing_policy",
        testonly = True,
        graph = [":lua_display_worker"],
        default_tasks = ["$lua/worker", "$runtime/input", "$runtime/nfc"],
        policies_json = "{}",
        policy_label = "display_worker_missing_policy",
        tags = ["manual"],
    )
    _missing_test(
        name = "display_worker_missing_policy_test",
        target_under_test = ":display_worker_missing_policy",
        target_compatible_with = compatible,
    )
