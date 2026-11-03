"""Pinned Bun runtime for building and testing the public npm CLI."""

_VERSION = "1.3.9"
_ARCHIVES = {
    "darwin-arm64": ("bun-darwin-aarch64", "cde6a4edf19cf64909158fa5a464a12026fd7f0d79a4a950c10cf0af04266d85"),
    "darwin-x64": ("bun-darwin-x64", "588f4a48740b9a0c366a00f878810ab3ab5e6734d29b7c3cbdd9484b74a007de"),
    "linux-arm64": ("bun-linux-aarch64", "a2c2862bcc1fd1c0b3a8dcdc8c7efb5e2acd871eb20ed2f17617884ede81c844"),
    "linux-x64": ("bun-linux-x64-baseline", "104d4d037f4b35e10215c0507e1779691f39c57bd91ddeefe11cad781e3fc4b9"),
    "windows-x64": ("bun-windows-x64-baseline", "cc22962b3dcc51f279f13bf843675e052479e64a323f85eb29442066cf3942fe"),
}

def _bun_repository_impl(ctx):
    host_os = ctx.os.name.lower()
    if host_os in ["mac os x", "macos"]:
        host_os = "darwin"
    elif host_os.startswith("windows"):
        host_os = "windows"
    arch = ctx.os.arch.lower()
    if arch in ["aarch64", "arm64"]:
        arch = "arm64"
    elif arch in ["amd64", "x86_64"]:
        arch = "x64"
    host = host_os + "-" + arch
    if host not in _ARCHIVES:
        fail("unsupported Bun host: " + host)
    archive, digest = _ARCHIVES[host]
    ctx.download_and_extract(
        url = "https://github.com/oven-sh/bun/releases/download/bun-v%s/%s.zip" % (_VERSION, archive),
        sha256 = digest,
        stripPrefix = archive,
    )
    if host_os == "windows":
        ctx.file("BUILD.bazel", """exports_files(["bun.exe"])
alias(name = "bun", actual = "bun.exe", visibility = ["//visibility:public"])
""")
    else:
        ctx.file("BUILD.bazel", 'exports_files(["bun"])\n')

bun_repository = repository_rule(implementation = _bun_repository_impl)
