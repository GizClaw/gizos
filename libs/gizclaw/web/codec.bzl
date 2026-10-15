"""Compile the pinned upstream object codec into the GizClaw Web adapter."""

load("//tools/bazel/platforms:compatibility.bzl", "HOST_TOOL_COMPATIBILITY")

def _codec_node_impl(ctx):
    node = ctx.toolchains["@rules_nodejs//nodejs:runtime_toolchain_type"].nodeinfo.node
    output = ctx.actions.declare_file(ctx.label.name + ".exe")
    ctx.actions.symlink(output = output, target_file = node, is_executable = True)
    return [DefaultInfo(executable = output, files = depset([output]), runfiles = ctx.runfiles(files = [node]))]

_codec_node = rule(
    implementation = _codec_node_impl,
    executable = True,
    toolchains = ["@rules_nodejs//nodejs:runtime_toolchain_type"],
)

def _codec_source_impl(ctx):
    sources = [ctx.file.bundler, ctx.file.codec, ctx.file.method_map, ctx.file.adapter, ctx.file.template]
    ctx.actions.run(
        executable = ctx.executable.node,
        arguments = [file.path for file in sources] + [ctx.outputs.out.path, ctx.file.compiler.path, ctx.file.binding.path, ctx.outputs.types[0].dirname],
        inputs = sources + ctx.files.typescript + [ctx.file.compiler, ctx.file.binding],
        tools = [ctx.attr.node[DefaultInfo].files_to_run],
        outputs = [ctx.outputs.out] + ctx.outputs.types,
        mnemonic = "GizClawWebCodec",
        progress_message = "Bundling canonical GizClaw Web codec %{label}",
    )
    return [DefaultInfo(files = depset([ctx.outputs.out])), OutputGroupInfo(types = depset(ctx.outputs.types))]

_codec_source = rule(
    implementation = _codec_source_impl,
    attrs = {
        "node": attr.label(executable = True, cfg = "exec", mandatory = True),
        "bundler": attr.label(allow_single_file = True, mandatory = True),
        "codec": attr.label(allow_single_file = True, mandatory = True),
        "method_map": attr.label(allow_single_file = True, mandatory = True),
        "adapter": attr.label(allow_single_file = True, mandatory = True),
        "template": attr.label(allow_single_file = True, mandatory = True),
        "out": attr.output(mandatory = True),
        "compiler": attr.label(allow_single_file = True, mandatory = True),
        "typescript": attr.label(mandatory = True),
        "binding": attr.label(allow_single_file = True, mandatory = True),
        "types": attr.output_list(mandatory = True),
    },
)

def web_gizclaw_sdk_source(name, out):
    """Generate the actual C/JS adapter with an exec-configured Node runtime."""
    _codec_node(name = name + "_node", target_compatible_with = HOST_TOOL_COMPATIBILITY)
    _codec_source(
        name = name,
        node = ":" + name + "_node",
        bundler = Label("//libs/gizclaw/web:bundle_codec.mjs"),
        codec = Label("@h2_gizclaw_protocol//:payload_codec"),
        method_map = Label("@h2_gizclaw_protocol//:method_map"),
        adapter = Label("//libs/gizclaw/web:src/h2_web_gizclaw_sdk.js"),
        template = Label("//libs/gizclaw/web:src/h2_web_gizclaw_sdk.c.in"),
        out = out,
        compiler = Label("@h2_typescript//:lib/typescript.js"),
        typescript = Label("@h2_typescript//:runtime"),
        binding = Label("//libs/gizclaw/web:include/h2_web_gizclaw.d.ts"),
        types = ["sdk_types/h2_web_gizclaw.d.ts", "sdk_types/payload-codec.d.ts", "sdk_types/method-map.d.ts"],
    )
