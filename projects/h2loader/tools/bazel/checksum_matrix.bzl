"""Build both package formats for all four checksum transitions."""

load(":h2loader_tar_zlib.bzl", "FirmwareReleaseInfo")

def _checksum_matrix_impl(ctx):
    a = ctx.attr.base[FirmwareReleaseInfo]
    b = ctx.attr.alternate[FirmwareReleaseInfo]
    if a.role != "app" or b.role != "app" or a.board != b.board or a.target != b.target:
        fail("checksum matrix requires two Apps for the same board and target")
    outputs = []
    for family, suffix in (("tar_zlib", ".update.tar.zlib"), ("zlib_tar", ".update.tar")):
        for case in ("baseline", "unchanged", "app-only", "data-only", "both-changed"):
            outputs.append(ctx.actions.declare_file(ctx.label.name + "/" + family + "/" + case + suffix))
    receipt = ctx.actions.declare_file(ctx.label.name + "/fixtures.json")
    args = ctx.actions.args()
    for key, value in (("app-a", a.native.app_image.path), ("app-b", b.native.app_image.path),
                       ("app-path", a.native.app_path), ("board", a.board), ("target", a.target),
                       ("version-a", a.version), ("version-b", b.version), ("output", receipt.dirname)):
        args.add("--" + key, value)
    ctx.actions.run(executable = ctx.executable._writer, arguments = [args],
                    inputs = [a.native.app_image, b.native.app_image], outputs = outputs + [receipt],
                    mnemonic = "H2LoaderChecksumMatrix", tools = [ctx.executable._writer])
    return [DefaultInfo(files = depset(outputs + [receipt]))]

h2loader_checksum_matrix = rule(implementation = _checksum_matrix_impl, attrs = {
    "base": attr.label(mandatory = True, providers = [FirmwareReleaseInfo]),
    "alternate": attr.label(mandatory = True, providers = [FirmwareReleaseInfo]),
    "_writer": attr.label(default = "//projects/h2loader/tools/bazel:checksum_fixture_writer", cfg = "exec", executable = True),
})
