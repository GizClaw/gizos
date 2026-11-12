"""Exact caller-owned public API headers, with no vendor implementation inputs."""

def _api_impl(ctx):
    root = ctx.os.environ.get("ALIPAY_API_PATH", "")
    manifest = json.decode(ctx.read(ctx.attr.manifest))
    if root:
        root = ctx.path(root)
        digest_tool = ctx.which("shasum")
        if not digest_tool:
            fail("shasum is required to verify the Alipay public API headers")
        for path, digest in manifest.items():
            source = root.get_child(path)
            if not source.exists:
                fail("Alipay API header missing: %s" % source)
            ctx.watch(source)
            check = ctx.execute([digest_tool, "-a", "256", str(source)], quiet = True)
            if check.return_code or check.stdout.split(" ")[0] != digest:
                fail("Alipay API header differs from original delivery: %s" % path)
            ctx.symlink(source, path)
    else:
        for path in manifest:
            ctx.file(path, '#error "Alipay public API unavailable: set ALIPAY_API_PATH"\n')
    headers = sorted(manifest.keys())
    includes = sorted({path.rpartition("/")[0]: True for path in headers}.keys())
    ctx.file("BUILD.bazel", "\n".join([
        'load("@rules_cc//cc:defs.bzl", "cc_library")',
        'package(default_visibility = ["//visibility:public"])',
        "cc_library(name = \"headers\", hdrs = %s, includes = %s)" % (repr(headers), repr(includes)),
    ]) + "\n")

_alipay_api = repository_rule(
    implementation = _api_impl,
    attrs = {"manifest": attr.label(default = Label("//third_party/alipay:api_manifest.json"))},
    environ = ["ALIPAY_API_PATH"],
    local = True,
)

def _extension_impl(module_ctx):
    _alipay_api(name = "alipay_api")

alipay_api = module_extension(implementation = _extension_impl)
