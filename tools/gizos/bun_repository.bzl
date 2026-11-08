"""Pinned Bun runtime for building and testing the public npm CLI."""

def _bun_repository_impl(ctx):
    manifest = json.decode(ctx.read(ctx.attr._runtime_manifest))
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
    if host not in manifest["archives"]:
        fail("unsupported Bun host: " + host)
    asset = manifest["archives"][host]
    archive = asset["archive"]
    ctx.download_and_extract(
        url = manifest["url"].format(version = manifest["version"], archive = archive),
        sha256 = asset["sha256"],
        stripPrefix = archive,
    )
    if host_os == "windows":
        ctx.file("BUILD.bazel", """exports_files(["bun.exe"])
alias(name = "bun", actual = "bun.exe", visibility = ["//visibility:public"])
""")
    else:
        ctx.file("BUILD.bazel", 'exports_files(["bun"])\n')

bun_repository = repository_rule(
    implementation = _bun_repository_impl,
    attrs = {
        "_runtime_manifest": attr.label(
            default = Label("//tools/gizos:bun_runtime.json"),
            allow_single_file = True,
        ),
    },
)
