"""Build one npm package and a local Bazel entry point for the Bun CLI."""

load("//tools/bazel:firmware.bzl", "FirmwareVersionInfo")

def _package_impl(ctx):
    package = ctx.actions.declare_directory(ctx.label.name)
    manifest = ctx.actions.declare_file(ctx.label.name + ".package.json")
    args = ctx.actions.args()
    args.add(ctx.file._builder.path)
    args.add("--entry", ctx.file.entry.path)
    args.add("--package-json", ctx.file.package_json.path)
    args.add("--readme", ctx.file.readme.path)
    args.add("--license", ctx.file.license.path)
    args.add("--output", package.path)
    args.add("--manifest", manifest.path)
    args.add("--batch", ctx.attr._batch[FirmwareVersionInfo].value)
    ctx.actions.run(
        executable = ctx.executable._bun,
        arguments = [args],
        inputs = depset(
            [ctx.file._builder, ctx.file.entry, ctx.file.package_json, ctx.file.readme, ctx.file.license],
            transitive = [depset(ctx.files.srcs)],
        ),
        outputs = [package, manifest],
        mnemonic = "GizOSCLIPackage",
        env = {"PATH": "/usr/bin:/bin", "BUN_TELEMETRY_DISABLE": "1"},
    )
    return [
        DefaultInfo(files = depset([package])),
        OutputGroupInfo(manifest = depset([manifest])),
    ]

gizos_package = rule(
    implementation = _package_impl,
    attrs = {
        "entry": attr.label(allow_single_file = True, mandatory = True),
        "srcs": attr.label_list(allow_files = True),
        "package_json": attr.label(allow_single_file = True, mandatory = True),
        "readme": attr.label(allow_single_file = True, mandatory = True),
        "license": attr.label(allow_single_file = True, mandatory = True),
        "_builder": attr.label(default = ":build.ts", allow_single_file = True),
        "_batch": attr.label(default = "//tools/bazel:release_batch"),
        "_bun": attr.label(default = "@gizos_bun//:bun", allow_single_file = True, executable = True, cfg = "exec"),
    },
)

def _runfile(ctx, file):
    path = file.short_path
    return path[3:] if path.startswith("../") else ctx.workspace_name + "/" + path

def _cli_impl(ctx):
    output = ctx.actions.declare_file(ctx.label.name + ".sh")
    package = ctx.file.package
    ctx.actions.write(
        output,
        """#!/bin/sh
set -eu
root=${RUNFILES_DIR:-$0.runfiles}
exec "$root/%s" "$root/%s/bin/gizos.js" "$@"
""" % (_runfile(ctx, ctx.executable._bun), _runfile(ctx, package)),
        is_executable = True,
    )
    return [DefaultInfo(
        executable = output,
        runfiles = ctx.runfiles(files = [package, ctx.executable._bun]),
    )]

gizos_cli = rule(
    implementation = _cli_impl,
    executable = True,
    attrs = {
        "package": attr.label(allow_single_file = True, mandatory = True),
        "_bun": attr.label(default = "@gizos_bun//:bun", allow_single_file = True, executable = True, cfg = "exec"),
    },
)
