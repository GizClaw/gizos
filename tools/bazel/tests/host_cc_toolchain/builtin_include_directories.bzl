"""Write the builtin include directories of the resolved C/C++ toolchain to a file."""

load("@rules_cc//cc:find_cc_toolchain.bzl", "CC_TOOLCHAIN_ATTRS", "find_cpp_toolchain", "use_cc_toolchain")

def _cc_builtin_include_directories_impl(ctx):
    cc_toolchain = find_cpp_toolchain(ctx)
    output = ctx.actions.declare_file(ctx.label.name + ".txt")
    ctx.actions.write(
        output = output,
        content = "".join([directory + "\n" for directory in cc_toolchain.built_in_include_directories]),
    )
    return [DefaultInfo(
        files = depset([output]),
        runfiles = ctx.runfiles(files = [output]),
    )]

cc_builtin_include_directories = rule(
    implementation = _cc_builtin_include_directories_impl,
    doc = """Lists `cxx_builtin_include_directories` of the toolchain serving the target platform.

    Bazel accepts an absolute path in a compile action's dependency file only
    when it lies under one of these directories.
    """,
    attrs = CC_TOOLCHAIN_ATTRS,
    fragments = ["cpp"],
    toolchains = use_cc_toolchain(),
)
