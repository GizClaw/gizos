"""Explicit local encrypted recovery over existing key-free BK native outputs."""

load("//tools/bazel:bk7258.bzl", "Bk7258FirmwareInfo")

def _runfile(ctx, file):
    path = file.short_path
    return path[3:] if path.startswith("../") else ctx.workspace_name + "/" + path

def _quote(value):
    return "'" + value.replace("'", "'\\''") + "'"

def _impl(ctx):
    firmware = ctx.attr.firmware[Bk7258FirmwareInfo]
    if firmware.target != "bk7258" or not firmware.recovery_config:
        fail("encrypted BK recovery requires bk7258 native firmware with board-layout recovery_config")
    for char in ctx.attr.board.elems():
        if char not in "abcdefghijklmnopqrstuvwxyz0123456789_-":
            fail("invalid recovery board identity")
    if not ctx.attr.board or len(ctx.attr.board) > 95:
        fail("recovery board identity is required")
    executable = ctx.actions.declare_file(ctx.label.name)
    paths = [
        _runfile(ctx, ctx.executable._tool),
        _runfile(ctx, firmware.recovery_image),
        _runfile(ctx, firmware.partition_metadata),
        _runfile(ctx, firmware.recovery_config),
    ]
    ctx.actions.write(executable, """#!/bin/bash
set -eu
r="${RUNFILES_DIR:-$0.runfiles}"
exec "$r"/%s --source "$r"/%s --metadata "$r"/%s --config "$r"/%s --board '%s' --target bk7258 "$@"
""" % (_quote(paths[0]), _quote(paths[1]), _quote(paths[2]), _quote(paths[3]), ctx.attr.board), is_executable = True)
    runfiles = ctx.runfiles(files = [firmware.recovery_image, firmware.partition_metadata, firmware.recovery_config])
    runfiles = runfiles.merge(ctx.attr._tool[DefaultInfo].default_runfiles)
    return [DefaultInfo(executable = executable, runfiles = runfiles)]

h2loader_bk_recovery = rule(
    implementation = _impl,
    executable = True,
    attrs = {
        "board": attr.string(mandatory = True),
        "firmware": attr.label(mandatory = True, providers = [Bk7258FirmwareInfo]),
        "_tool": attr.label(default = ":encrypted_bk_recovery", executable = True, cfg = "exec"),
    },
    doc = "Build key-free inputs; run locally with --key-fd and --output-dir to encrypt and publish recovery only.",
)
