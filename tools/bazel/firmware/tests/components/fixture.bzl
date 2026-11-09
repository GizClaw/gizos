"""Reusable archive/source consumer receipt and focused analysis checks."""

load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts")
load("//tools/bazel/firmware:firmware_components.bzl", "FirmwareLibComponentInfo", "collect_firmware_components", "firmware_components_aspect")
load("//tools/bazel/firmware:tasks.bzl", "H2TasksInfo", "h2_tasks_aspect")

FixtureReceiptInfo = provider(
    doc = "Collected firmware source/archive records and their declared action inputs.",
    fields = {"components": "Collected component records.", "files": "Complete input depset."},
)

def _receipt_impl(ctx):
    components = collect_firmware_components(ctx.attr.deps)
    files = depset(
        direct = [component.archive for component in components if component.archive],
        transitive = [component.native_srcs for component in components] + [component.headers for component in components],
    )
    tasks = depset(transitive = [dependency[H2TasksInfo].tasks for dependency in ctx.attr.deps])
    metadata = {
        "components": [{
            "archive": component.archive.path if component.archive else None,
            "component_name": component.component_name,
            "generate_component": component.generate_component,
            "headers": sorted([header.path for header in component.headers.to_list()]),
            "include_roots": component.include_roots,
            "native_srcs": sorted([source.path for source in component.native_srcs.to_list()]),
        } for component in components],
        "tasks": sorted([task.name for task in tasks.to_list()]),
    }
    receipt = ctx.actions.declare_file(ctx.label.name + ".json")
    args = ctx.actions.args()
    args.add("--metadata", json.encode(metadata))
    args.add("--output", receipt.path)
    ctx.actions.run(
        executable = ctx.executable._receipt_tool,
        arguments = [args],
        inputs = files,
        outputs = [receipt],
        mnemonic = "FirmwareComponentReceipt",
    )
    return [
        DefaultInfo(files = depset([receipt])),
        FixtureReceiptInfo(components = components, files = files),
    ]

firmware_component_receipt = rule(
    implementation = _receipt_impl,
    attrs = {
        "deps": attr.label_list(aspects = [firmware_components_aspect, h2_tasks_aspect]),
        "_receipt_tool": attr.label(
            default = Label(":receipt_tool"),
            executable = True,
            cfg = "exec",
        ),
    },
)

def _metadata_test_impl(ctx):
    env = analysistest.begin(ctx)
    target = analysistest.target_under_test(env)
    component = target[FirmwareLibComponentInfo]
    asserts.equals(env, ctx.attr.expected_archives, len(component.archives.to_list()))
    expected_sources = ["portable.c"] if ctx.attr.has_native_sources else []
    asserts.equals(env, expected_sources, [source.basename for source in component.native_srcs.to_list()])
    headers = [header.basename for header in component.headers.to_list()]
    asserts.true(env, "portable.h" in headers)
    default_paths = [file.path for file in target[DefaultInfo].files.to_list()]
    if ctx.attr.has_native_sources:
        asserts.true(env, "explicit.h" in headers)
        for file in component.native_srcs.to_list() + component.headers.to_list():
            asserts.true(env, file.path in default_paths, "missing declared input: " + file.path)
    else:
        asserts.equals(env, [archive.path for archive in component.archives.to_list()], default_paths)
    return analysistest.end(env)

component_metadata_test = analysistest.make(
    _metadata_test_impl,
    attrs = {
        "expected_archives": attr.int(mandatory = True),
        "has_native_sources": attr.bool(default = True),
    },
)

def _failure_test_impl(ctx):
    env = analysistest.begin(ctx)
    asserts.expect_failure(env, ctx.attr.expected_message)
    return analysistest.end(env)

component_failure_test = analysistest.make(
    _failure_test_impl,
    expect_failure = True,
    attrs = {"expected_message": attr.string(mandatory = True)},
)

def _sdk_action_test_impl(ctx):
    env = analysistest.begin(ctx)
    actions = [action for action in analysistest.target_actions(env) if action.mnemonic == ctx.attr.mnemonic]
    asserts.equals(env, 1, len(actions))
    if actions:
        action = actions[0]
        inputs = [file.basename for file in action.inputs.to_list()]
        for basename in ["portable.c", "portable.h", "explicit.h"]:
            asserts.true(env, basename in inputs, "missing SDK action input: " + basename)
        argv = action.argv
        sources = [argv[index + 1] for index in range(len(argv) - 1) if argv[index] == "--native-component-source"]
        asserts.equals(env, 1, len(sources))
        if sources:
            asserts.true(env, sources[0].startswith(ctx.attr.source_owner + "="))
            asserts.true(env, sources[0].endswith("/portable.c"))
        generated = [argv[index + 1] for index in range(len(argv) - 1) if argv[index] == "--generate-prebuilt-component"]
        asserts.equals(env, ["h2_firmware_lib"], generated)
        archives = [argv[index + 1] for index in range(len(argv) - 1) if argv[index] == "--prebuilt-component"]
        asserts.equals(env, 2, len(archives))
    return analysistest.end(env)

_SDK_TEST_ATTRS = {
    "mnemonic": attr.string(mandatory = True),
    "source_owner": attr.string(mandatory = True),
}

sdk_action_test = analysistest.make(
    _sdk_action_test_impl,
    attrs = _SDK_TEST_ATTRS,
)

def sdk_action_tests(name, target, mnemonic, source_owner):
    """Inspect real SDK actions with --define=h2_ci_graph=true, without an SDK."""
    sdk_action_test(
        name = name + "_test",
        target_under_test = target,
        mnemonic = mnemonic,
        source_owner = source_owner,
        target_compatible_with = select({
            Label("//tools/bazel/platforms:ci_graph"): [],
            "//conditions:default": ["@platforms//:incompatible"],
        }),
    )
