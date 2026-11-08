"""Generate the firmware runtime byte interface from its shared ABI identity."""

def _source_impl(ctx):
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [
            "--protocol",
            ctx.file.protocol.path,
            "--template",
            ctx.file.template.path,
            "--output",
            ctx.outputs.out.path,
        ],
        inputs = [ctx.file.protocol, ctx.file.template],
        tools = [ctx.attr._tool[DefaultInfo].files_to_run],
        outputs = [ctx.outputs.out],
        mnemonic = "GizClawFirmwareTransport",
    )
    return [DefaultInfo(files = depset([ctx.outputs.out]))]

firmware_transport_source = rule(
    implementation = _source_impl,
    attrs = {
        "_tool": attr.label(default = ":transport_generator", executable = True, cfg = "exec"),
        "protocol": attr.label(allow_single_file = True, mandatory = True),
        "template": attr.label(allow_single_file = True, mandatory = True),
        "out": attr.output(mandatory = True),
    },
)
