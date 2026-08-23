"""Bazel-owned assembly of the complete firmware release bundle."""

load(":firmware.bzl", "FirmwareVersionInfo")


def _impl(ctx):
    output = ctx.actions.declare_directory(ctx.label.name)
    version = ctx.attr._firmware_version[FirmwareVersionInfo].value
    args = ctx.actions.args()
    args.add("--output", output.path)
    args.add("--version", version)
    args.add_all(ctx.files.srcs, before_each = "--input")
    ctx.actions.run(
        executable = ctx.executable._assembler,
        arguments = [args],
        inputs = ctx.files.srcs,
        outputs = [output],
        tools = [ctx.executable._assembler],
        mnemonic = "FirmwareReleaseBundle",
        progress_message = "Assembling validated firmware release bundle",
    )
    return [DefaultInfo(files = depset([output]))]


firmware_release_bundle = rule(
    implementation = _impl,
    attrs = {
        "_assembler": attr.label(
            default = "//tools/bazel:release_bundle_assembler",
            cfg = "exec",
            executable = True,
        ),
        "_firmware_version": attr.label(default = "//tools/bazel:firmware_version"),
        "srcs": attr.label_list(allow_files = True),
    },
)
