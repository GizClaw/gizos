"""Target-owned task-policy components for native firmware.

Modules declare the task names they start with ``h2_tasks`` and carry that
declaration in their own dependency list, so the firmware's dependency graph
yields every task in the image. A target answers with one policy row per
task; the component that installs them is generated from that table.
"""

load("@rules_cc//cc:defs.bzl", "cc_test")
load("//tools/bazel:cc_options.bzl", "H2_C11_OPTS", "H2_WARNING_COPTS")
load("//tools/bazel:native_component.bzl", "firmware_native_component")
load(
    "//tools/bazel:target_task_policy_codegen.bzl",
    "encode_default_policy",
    "encode_policies",
    "task_policy_audit",
    "task_policy_codegen",
)

_HOST_COMPATIBILITY = select({
    Label("//tools/bazel/platforms:host_linux_target_linux"): [],
    Label("//tools/bazel/platforms:host_macos_target_macos"): [],
    Label("//tools/bazel/platforms:host_windows_target_windows"): [],
    "//conditions:default": ["@platforms//:incompatible"],
})

_ESP_PAL_CORE = Label("//native_component_src/esp-idf6.x/h2_pal_core")
_ESP_POLICY_TEST_SDK = Label("//native_component_src/esp-idf6.x/h2_target_task_policy:test_sdk")
_BK_AP_PAL_CORE = Label("//native_component_src/bk7258/ap/h2_pal_core")
_BK_AP_POLICY_TEST_SDK = Label("//native_component_src/bk7258/ap/h2_target_task_policy:test_sdk")
_BK_CP_PAL_CORE = Label("//native_component_src/bk7258/cp/h2_pal_core")
_BK_CP_POLICY_TEST_SDK = Label("//native_component_src/bk7258/cp/h2_target_task_policy:test_sdk")
_PAL = Label("//libs/pal")
_TRIE = Label("//libs/trie")

def _target_directory(relative_directory):
    package = native.package_name()
    if not relative_directory:
        return package
    return package + "/" + relative_directory if package else relative_directory

def _generate(
        name,
        unit,
        directory,
        source_name,
        graph,
        policies,
        default_policy,
        allocator = ""):
    """Declares the codegen for one unit and returns its source file labels."""
    label = "//" + native.package_name() + ":" + name
    policies_json, default_tasks = encode_policies(label, unit, policies)
    source = directory + "/" + source_name
    test_source = directory + "/tests/test_" + source_name
    task_policy_codegen(
        name = name + "_codegen",
        allocator = allocator,
        default_policy_json = encode_default_policy(label, unit, default_policy),
        default_tasks = default_tasks,
        policies_json = policies_json,
        source_name = source,
        target_directory = _target_directory(""),
        test_source_name = test_source,
        unit = unit,
    )

    # The table alone decides the routes, so only this audit needs the firmware
    # graph; keeping it off the generator leaves the host test free of it.
    task_policy_audit(
        name = name + "_audit",
        default_tasks = default_tasks,
        graph = graph,
        policies_json = policies_json,
        policy_label = label,
    )
    native.filegroup(
        name = name + "_source",
        srcs = [":" + name + "_codegen"],
        output_group = "source",
    )
    native.filegroup(
        name = name + "_test_source",
        srcs = [":" + name + "_codegen"],
        output_group = "test_source",
    )
    for group in ["header", "cmake"]:
        native.filegroup(
            name = name + "_" + group,
            srcs = [":" + name + "_codegen"],
            output_group = group,
        )
    return ":" + name + "_source", ":" + name + "_test_source"

def esp_target_task_policy(
        name = "task_policy",
        directory = "task_policy",
        graph = [],
        policies = [],
        default_policy = ""):
    """Declares one ESP policy owned by the calling firmware target package.

    Every task declared anywhere in ``graph`` needs a row in ``policies``, and
    each row states its own priority, core, stack size and stack region, so a
    task that arrives with a new dependency fails analysis instead of silently
    inheriting a budget that was never weighed for it.

    Args:
      name: Name of the generated native component target.
      directory: Package-relative generated output directory for the policy component.
      graph: The firmware dependency graph whose tasks this policy serves.
      policies: One row per task, ``"<task>  <priority>  <core>  <stack>
        <region>"``, or ``"<task>  default"`` for a task deliberately served
        by the default policy.
      default_policy: ``"<priority>  <core>  <stack>  <region>"`` served to
        task names without a row of their own.
    """
    header = ":" + name + "_header"
    source, test_source = _generate(
        name = name,
        unit = "esp",
        directory = directory,
        source_name = "h2_esp_target_task_policy.c",
        graph = graph,
        policies = policies,
        default_policy = default_policy,
    )
    firmware_native_component(
        name = name,
        hdrs = [header],
        srcs = [source],
        component_name = "h2_esp_target_task_policy",
        data = [
            ":" + name + "_cmake",
            ":" + name + "_audit",
        ],
        deps = [_ESP_PAL_CORE],
    )
    cc_test(
        name = name + "_test",
        srcs = [
            header,
            source,
            test_source,
        ],
        conlyopts = H2_C11_OPTS,
        copts = H2_WARNING_COPTS,
        includes = [directory],
        target_compatible_with = _HOST_COMPATIBILITY,
        deps = [
            _PAL,
            _TRIE,
            _ESP_POLICY_TEST_SDK,
        ],
    )

def bk7258_target_task_policy(
        ap_name = "ap_task_policy",
        cp_name = "cp_task_policy",
        directory = "task_policy",
        graph = [],
        ap_policies = [],
        ap_default_policy = "",
        ap_allocator = "psram",
        cp_default_policy = ""):
    """Declares the AP and CP policies owned by one BK7258 firmware target.

    The AP unit runs the image's tasks and answers the same kind of policy
    table as an ESP target. The CP unit runs SDK-owned tasks only, so it
    serves one policy to every name and takes no table.

    Args:
      ap_name: Name of the generated AP native component target.
      cp_name: Name of the generated CP native component target.
      directory: Package-relative generated output directory for the policy components.
      graph: The firmware dependency graph whose tasks the AP policy serves.
      ap_policies: One row per AP task. See ``esp_target_task_policy``.
      ap_default_policy: ``"<priority>  <core>  <stack>  <region>"`` served to
        AP task names without a row of their own.
      ap_allocator: Memory the AP allocates task stacks from.
      cp_default_policy: ``"<priority>  <stack>  <region>"`` served to every
        CP task name.
    """
    ap_directory = directory + "/ap"
    cp_directory = directory + "/cp"
    ap_header = ":" + ap_name + "_header"
    cp_header = ":" + cp_name + "_header"
    ap_source, ap_test_source = _generate(
        name = ap_name,
        unit = "ap",
        directory = ap_directory,
        source_name = "h2_bk_target_task_policy.c",
        graph = graph,
        policies = ap_policies,
        default_policy = ap_default_policy,
        allocator = ap_allocator,
    )
    cp_source, cp_test_source = _generate(
        name = cp_name,
        unit = "cp",
        directory = cp_directory,
        source_name = "h2_bk_target_task_policy.c",
        graph = [],
        policies = [],
        default_policy = cp_default_policy,
    )
    firmware_native_component(
        name = ap_name,
        hdrs = [ap_header],
        srcs = [ap_source],
        component_name = "h2_bk_target_task_policy",
        data = [
            ":" + ap_name + "_cmake",
            ":" + ap_name + "_audit",
        ],
        execution_unit = "ap",
        deps = [_BK_AP_PAL_CORE],
    )
    firmware_native_component(
        name = cp_name,
        hdrs = [cp_header],
        srcs = [cp_source],
        component_name = "h2_bk_target_task_policy",
        data = [":" + cp_name + "_cmake"],
        execution_unit = "cp",
        deps = [_BK_CP_PAL_CORE],
    )
    cc_test(
        name = ap_name + "_test",
        srcs = [
            ap_header,
            ap_source,
            ap_test_source,
        ],
        conlyopts = H2_C11_OPTS,
        copts = H2_WARNING_COPTS,
        includes = [ap_directory],
        target_compatible_with = _HOST_COMPATIBILITY,
        deps = [
            _PAL,
            _TRIE,
            _BK_AP_POLICY_TEST_SDK,
        ],
    )
    cc_test(
        name = cp_name + "_test",
        srcs = [
            cp_header,
            cp_source,
            cp_test_source,
        ],
        conlyopts = H2_C11_OPTS,
        copts = H2_WARNING_COPTS,
        includes = [cp_directory],
        target_compatible_with = _HOST_COMPATIBILITY,
        deps = [
            _PAL,
            _BK_CP_POLICY_TEST_SDK,
        ],
    )
