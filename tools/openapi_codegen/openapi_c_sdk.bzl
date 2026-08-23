"""Bazel rule for generating a PAL-backed C SDK from OpenAPI JSON."""

load("@rules_cc//cc:defs.bzl", "cc_library")
load("//tools/bazel:cc_options.bzl", "H2_C11_OPTS", "H2_WARNING_COPTS")

def _openapi_c_sources_impl(ctx):
    header = ctx.outputs.header
    source = ctx.outputs.source
    schema = ctx.actions.declare_file(ctx.label.name + ".schema.json")
    ctx.actions.expand_template(
        output = schema,
        substitutions = {},
        template = ctx.file.schema,
    )
    arguments = ctx.actions.args()
    arguments.add("--schema", schema)
    arguments.add("--package", ctx.attr.package_name)
    arguments.add("--header-output", header.path)
    arguments.add("--source-output", source.path)
    ctx.actions.run(
        arguments = [arguments],
        executable = ctx.executable.tool,
        inputs = [schema],
        mnemonic = "H2OpenAPICCodegen",
        outputs = [header, source],
        progress_message = "Generating PAL-backed C SDK %{label}",
        tools = [ctx.executable.tool],
    )
    return DefaultInfo(files = depset([header, source]))

openapi_c_sources = rule(
    implementation = _openapi_c_sources_impl,
    attrs = {
        "package_name": attr.string(mandatory = True),
        "header": attr.output(mandatory = True),
        "schema": attr.label(allow_single_file = [".json"], mandatory = True),
        "source": attr.output(mandatory = True),
        "tool": attr.label(
            default = Label("//tools/openapi_codegen:openapi_codegen"),
            executable = True,
            cfg = "exec",
        ),
    },
)

def openapi_c_sdk(name, schema, package_name, deps, visibility = None, tags = None):
    """Generates and compiles a target-independent PAL-backed C SDK."""
    generated_name = name + "_generated"
    openapi_c_sources(
        name = generated_name,
        header = "h2_%s_api.h" % package_name,
        package_name = package_name,
        schema = schema,
        source = "h2_%s_api.c" % package_name,
        visibility = ["//visibility:private"],
    )
    cc_library(
        name = name,
        hdrs = ["h2_%s_api.h" % package_name],
        srcs = ["h2_%s_api.c" % package_name],
        conlyopts = H2_C11_OPTS,
        copts = H2_WARNING_COPTS,
        deps = deps,
        strip_include_prefix = ".",
        tags = tags,
        visibility = visibility,
    )
