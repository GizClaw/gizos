"""Build the optional npm runtime SDK; the firmware never depends on this target."""

load("//tools/bazel/platforms:compatibility.bzl", "HOST_TOOL_COMPATIBILITY")

def _node_impl(ctx):
    node = ctx.toolchains["@rules_nodejs//nodejs:runtime_toolchain_type"].nodeinfo.node
    output = ctx.actions.declare_file(ctx.label.name + ".exe")
    ctx.actions.symlink(output = output, target_file = node, is_executable = True)
    return [DefaultInfo(executable = output, files = depset([output]), runfiles = ctx.runfiles(files = [node]))]

_node = rule(
    implementation = _node_impl,
    executable = True,
    toolchains = ["@rules_nodejs//nodejs:runtime_toolchain_type"],
)

def _sdk_impl(ctx):
    inputs = [
        ctx.file.bundler,
        ctx.file.codec,
        ctx.file.method_map,
        ctx.file.adapter,
        ctx.file.protocol,
        ctx.file.compiler,
        ctx.file.binding,
        ctx.file.license,
    ]
    ctx.actions.run(
        executable = ctx.executable.node,
        arguments = [
            ctx.file.bundler.path,
            ctx.file.codec.path,
            ctx.file.method_map.path,
            ctx.file.adapter.path,
            ctx.file.protocol.path,
            ctx.outputs.out.path,
            ctx.file.compiler.path,
            ctx.file.binding.path,
            ctx.outputs.types[0].dirname,
            ctx.file.license.path,
            ctx.outputs.license_out.path,
        ],
        inputs = inputs + ctx.files.typescript,
        tools = [ctx.attr.node[DefaultInfo].files_to_run],
        outputs = [ctx.outputs.out, ctx.outputs.license_out] + ctx.outputs.types,
        mnemonic = "GizClawFirmwareSDK",
    )
    return [
        DefaultInfo(files = depset([ctx.outputs.out])),
        OutputGroupInfo(types = depset(ctx.outputs.types), license = depset([ctx.outputs.license_out])),
    ]

_sdk = rule(
    implementation = _sdk_impl,
    attrs = {
        "node": attr.label(executable = True, cfg = "exec", mandatory = True),
        "bundler": attr.label(allow_single_file = True, mandatory = True),
        "codec": attr.label(allow_single_file = True, mandatory = True),
        "method_map": attr.label(allow_single_file = True, mandatory = True),
        "adapter": attr.label(allow_single_file = True, mandatory = True),
        "protocol": attr.label(allow_single_file = True, mandatory = True),
        "compiler": attr.label(allow_single_file = True, mandatory = True),
        "binding": attr.label(allow_single_file = True, mandatory = True),
        "license": attr.label(allow_single_file = True, mandatory = True),
        "typescript": attr.label(mandatory = True),
        "out": attr.output(mandatory = True),
        "license_out": attr.output(mandatory = True),
        "types": attr.output_list(mandatory = True),
    },
)

def firmware_sdk(name):
    """Compile canonical types and JS only for explicit WASM runtime clients."""
    _node(name = name + "_node", target_compatible_with = HOST_TOOL_COMPATIBILITY)
    _sdk(
        name = name,
        node = ":" + name + "_node",
        bundler = "bundle_sdk.mjs",
        codec = "@h2_gizclaw_protocol//:payload_codec",
        method_map = "@h2_gizclaw_protocol//:method_map",
        adapter = "src/firmware.js",
        protocol = "//libs/gizclaw/web:protocol.json",
        compiler = "@h2_typescript//:lib/typescript.js",
        typescript = "@h2_typescript//:runtime",
        binding = "include/firmware.d.ts",
        license = "@h2_gizclaw_protocol//:LICENSE",
        out = "firmware.js",
        license_out = "GIZCLAW_PROTOCOL_LICENSE",
        types = ["types/firmware.d.ts", "types/payload-codec.d.ts", "types/method-map.d.ts"],
        target_compatible_with = HOST_TOOL_COMPATIBILITY,
    )
