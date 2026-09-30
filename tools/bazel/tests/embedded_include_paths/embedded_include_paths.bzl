"""Compile probe sources with the selected embedded C/C++ toolchain and check their header paths."""

load("@rules_cc//cc:action_names.bzl", "ACTION_NAMES")
load("@rules_cc//cc:find_cc_toolchain.bzl", "CC_TOOLCHAIN_ATTRS", "find_cpp_toolchain", "use_cc_toolchain")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")

def _compile(ctx, cc_toolchain, feature_configuration, action_name, source):
    stem = "%s/%s" % (ctx.label.name, source.basename)
    output = ctx.actions.declare_file(stem + ".o")
    dependency_file = ctx.actions.declare_file(stem + ".d")
    variables = cc_common.create_compile_variables(
        cc_toolchain = cc_toolchain,
        feature_configuration = feature_configuration,
        source_file = source.path,
        output_file = output.path,
        variables_extension = {"dependency_file": dependency_file.path},
    )
    ctx.actions.run(
        executable = cc_common.get_tool_for_action(
            feature_configuration = feature_configuration,
            action_name = action_name,
        ),
        arguments = cc_common.get_memory_inefficient_command_line(
            feature_configuration = feature_configuration,
            action_name = action_name,
            variables = variables,
        ),
        env = cc_common.get_environment_variables(
            feature_configuration = feature_configuration,
            action_name = action_name,
            variables = variables,
        ),
        inputs = depset([source], transitive = [cc_toolchain.all_files]),
        mnemonic = "EmbeddedIncludePathProbe",
        outputs = [output, dependency_file],
        progress_message = "Compiling include path probe %s" % source.short_path,
    )
    return dependency_file

def _embedded_include_paths_check_impl(ctx):
    cc_toolchain = find_cpp_toolchain(ctx)
    feature_configuration = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = cc_toolchain,
        requested_features = ctx.features,
        unsupported_features = ctx.disabled_features,
    )
    dependency_files = [
        _compile(ctx, cc_toolchain, feature_configuration, ACTION_NAMES.c_compile, ctx.file.c_src),
        _compile(ctx, cc_toolchain, feature_configuration, ACTION_NAMES.cpp_compile, ctx.file.cxx_src),
    ]
    report = ctx.actions.declare_file(ctx.label.name + ".txt")
    arguments = ctx.actions.args()
    arguments.add_all(cc_toolchain.built_in_include_directories, before_each = "--builtin-include-directory")
    arguments.add_all(dependency_files, before_each = "--dependency-file")
    arguments.add("--output", report)
    ctx.actions.run(
        arguments = [arguments],
        executable = ctx.executable._checker,
        inputs = dependency_files,
        mnemonic = "EmbeddedIncludePathCheck",
        outputs = [report],
        progress_message = "Checking embedded include paths for %{label}",
    )
    return [DefaultInfo(files = depset([report]))]

embedded_include_paths_check = rule(
    implementation = _embedded_include_paths_check_impl,
    doc = """Fails the build when the embedded toolchain names absolute system header paths.

    Absolute builtin include directories or dependency-file entries tie compile
    outputs to one Bazel output base, so a disk or remote cache hit from another
    workspace fails Bazel's header inclusion check.
    """,
    attrs = {
        "c_src": attr.label(allow_single_file = [".c"], mandatory = True),
        "cxx_src": attr.label(allow_single_file = [".cc"], mandatory = True),
        "_checker": attr.label(
            cfg = "exec",
            default = Label(":check_include_paths"),
            executable = True,
        ),
    } | CC_TOOLCHAIN_ATTRS,
    fragments = ["cpp"],
    toolchains = use_cc_toolchain(),
)
