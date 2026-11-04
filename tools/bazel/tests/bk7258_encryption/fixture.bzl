"""Native firmware fixtures without SDK or H2Loader dependencies."""

load("//tools/bazel:bk7258.bzl", "Bk7258FirmwareInfo")

def _native_fixture_impl(ctx):
    source = ctx.actions.declare_file(ctx.label.name + "/native.bin")
    metadata = ctx.actions.declare_directory(ctx.label.name + "/metadata")
    args = ctx.actions.args()
    args.add("--image", ctx.attr.image)
    args.add("--source", source.path)
    args.add("--metadata", metadata.path)
    ctx.actions.run(
        executable = ctx.executable._writer,
        arguments = [args],
        outputs = [source, metadata],
        mnemonic = "NativeBkEncryptionFixture",
    )
    return [
        DefaultInfo(files = depset([source, metadata])),
        Bk7258FirmwareInfo(
            managed_app_image = source,
            recovery_image = source,
            partition_metadata = metadata,
            target = "bk7258",
        ),
    ]

native_fixture = rule(
    implementation = _native_fixture_impl,
    attrs = {
        "image": attr.string(mandatory = True, values = ["app", "full"]),
        "_writer": attr.label(default = Label(":fixture_writer"), executable = True, cfg = "exec"),
    },
)
