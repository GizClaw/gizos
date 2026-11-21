"""Optional, verified Rockchip chip firmware repositories for Bzlmod consumers."""

_RKBIN = "https://raw.githubusercontent.com/rockchip-linux/rkbin/"

def _rkbin_impl(ctx):
    profile = json.decode(ctx.read(ctx.attr.profile))
    downloads = []
    for name, file in profile["files"].items():
        downloads.append(ctx.download(
            url = _RKBIN + profile["commit"] + "/" + file["path"],
            output = name,
            sha256 = file["sha256"],
            block = False,
        ))
    for download in downloads:
        download.wait()
    ctx.file("BUILD.bazel", "exports_files(%s)\n" % repr(sorted(profile["files"].keys())))

_rkbin = repository_rule(
    implementation = _rkbin_impl,
    attrs = {"profile": attr.label(mandatory = True, allow_single_file = True)},
)

def _rockchip_repositories_impl(module_ctx):
    _rkbin(
        name = "gizos_rk3326_legacy_rkbin",
        profile = Label("//tools/bazel/native/versions:rk3326_legacy_rkbin.json"),
    )
    return module_ctx.extension_metadata(reproducible = True)

rockchip_repositories = module_extension(
    implementation = _rockchip_repositories_impl,
    doc = "Provides the pinned RK3326 legacy rkbin inputs and Rockchip license.",
)
