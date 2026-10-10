"""Generate native USB fixtures without an ambient compiler or shell."""
def _fixture_impl(ctx):
    output = ctx.actions.declare_file(ctx.label.name + ".c")
    arguments = ctx.actions.args()
    arguments.add("--emit-kind", ctx.attr.kind)
    arguments.add("--output", output.path)
    ctx.actions.run(
        executable = ctx.attr.generator[DefaultInfo].files_to_run,
        arguments = [arguments],
        outputs = [output],
        mnemonic = "MosaicoUsbFixture",
    )
    return [DefaultInfo(files = depset([output]))]

loader_usb_fixture = rule(
    implementation = _fixture_impl,
    attrs = {
        "kind": attr.string(mandatory = True, values = ["config", "callbacks"]),
        "generator": attr.label(mandatory = True, executable = True, cfg = "exec"),
    },
)
