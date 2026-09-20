"""Deterministic npm tarballs and the snapshot Release npm index."""

load(":firmware.bzl", "FirmwareVersionInfo")

NpmReleaseInfo = provider(
    doc = "One npm tarball whose basename is resolved from its manifest at build time.",
    fields = {
        "directory": "Tree artifact containing exactly one .tgz.",
        "manifest": "The package.json used to validate and name the tarball.",
    },
)

def _tarball_impl(ctx):
    package = ctx.file.package
    if not package.is_directory:
        fail("package must be an npm_package tree artifact")

    # Bazel analysis cannot read the manifest; resolve the sole .tgz basename
    # inside a tree artifact during execution instead of duplicating its version.
    output = ctx.actions.declare_directory(ctx.label.name)
    args = ctx.actions.args()
    args.add("pack")
    args.add("--package", package.path)
    args.add("--manifest", ctx.file.manifest.path)
    args.add("--output", output.path)
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [args],
        inputs = [package, ctx.file.manifest],
        outputs = [output],
        mnemonic = "NpmReleaseTarball",
        progress_message = "Packing deterministic npm release tarball %{label}",
    )
    return [
        DefaultInfo(files = depset([output])),
        NpmReleaseInfo(directory = output, manifest = ctx.file.manifest),
    ]

npm_release_tarball = rule(
    implementation = _tarball_impl,
    attrs = {
        "_tool": attr.label(
            default = "//tools/bazel:npm_release_tool",
            cfg = "exec",
            executable = True,
        ),
        "manifest": attr.label(allow_single_file = [".json"], mandatory = True),
        "package": attr.label(allow_single_file = True, mandatory = True),
    },
)

def _bundle_impl(ctx):
    output = ctx.actions.declare_directory(ctx.label.name)
    args = ctx.actions.args()
    args.add("bundle")
    args.add("--output", output.path)
    args.add("--version", ctx.attr._release_version[FirmwareVersionInfo].value)
    inputs = []
    for dep in ctx.attr.packages:
        info = dep[NpmReleaseInfo]
        args.add("--package")
        args.add(info.directory.path)
        args.add(info.manifest.path)
        inputs.extend([info.directory, info.manifest])
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [args],
        inputs = depset(inputs),
        outputs = [output],
        mnemonic = "NpmReleaseBundle",
        progress_message = "Assembling validated npm release bundle",
    )
    return [DefaultInfo(files = depset([output]))]

npm_release_bundle = rule(
    implementation = _bundle_impl,
    attrs = {
        "_release_version": attr.label(default = "//tools/bazel:release_version"),
        "_tool": attr.label(
            default = "//tools/bazel:npm_release_tool",
            cfg = "exec",
            executable = True,
        ),
        "packages": attr.label_list(providers = [NpmReleaseInfo], mandatory = True),
    },
)
