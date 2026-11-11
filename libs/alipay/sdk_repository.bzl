"""Caller-supplied, byte-verified Alipay binary SDK; never vendor the delivery."""

_MODULES = [
    "app/bind_net",
    "app/setting",
    "fwk/common",
    "fwk/bind_status",
    "fwk/crypt",
    "fwk/secure_mode_ecdh",
    "fwk/network",
    "share/common",
    "3rdparty/cJSON",
    "3rdparty/uEcc",
    "3rdparty/mbedtls",
    "3rdparty/protobuf-c",
    "3rdparty/sm3",
    "3rdparty/bitmap",
]

def _sdk_impl(ctx):
    root = ctx.os.environ.get("ALIPAY_SDK_PATH", "")
    manifest = json.decode(ctx.read(ctx.attr.manifest))
    digest_tool = ctx.which("shasum")
    if not digest_tool:
        fail("shasum is required to verify the caller-supplied Alipay SDK")
    identity = ctx.execute([digest_tool, "-a", "256", str(ctx.path(ctx.attr.manifest))], quiet = True)
    if identity.return_code:
        fail("cannot hash Alipay SDK manifest")
    manifest_digest = identity.stdout.split(" ")[0]
    ctx.file("h2_alipay_sdk_identity.h", '#define H2_ALIPAY_SDK_MANIFEST_SHA256 "%s"\n' % manifest_digest)
    if root:
        root = ctx.path(root)
        for path, digest in manifest.items():
            source = root.get_child(path)
            if not source.exists:
                fail("Alipay SDK input missing: %s" % source)
            check = ctx.execute([digest_tool, "-a", "256", str(source)], quiet = True)
            if check.return_code or check.stdout.split(" ")[0] != digest:
                fail("Alipay SDK input differs from original delivery: %s" % path)
            ctx.symlink(source, path)
    lines = ['load("@rules_cc//cc:defs.bzl", "cc_import", "cc_library")', 'package(default_visibility = ["//visibility:public"])']
    deps = []
    archives = []
    for module in _MODULES:
        name = module.replace("/", "_")
        archive = [path for path in manifest if path.startswith(module + "/lib/")][0]
        archives.append(archive)
        if not root:
            # Host graphs can load this package; an exact SDK build fails.
            lines.append("genrule(name = %s, outs = [%s], cmd = %s)" % (
                repr("missing_" + name),
                repr(archive),
                repr("echo 'Alipay SDK unavailable; set ALIPAY_SDK_PATH to the original output directory' >&2; exit 1"),
            ))
        lines.append('cc_import(name = "%s", static_library = "%s")' % (name, archive))
        deps.append(":" + name)
    headers = [path for path in manifest if path.endswith(".h")] + ["h2_alipay_sdk_identity.h"]
    if not root:
        for path in headers:
            if path == "h2_alipay_sdk_identity.h":
                continue
            ctx.file(path, '#error "Alipay SDK missing: set ALIPAY_SDK_PATH"\n')
    includes = sorted({path.rpartition("/")[0] or ".": True for path in headers}.keys())
    lines.append("cc_library(name = \"bind_net\", hdrs = %s, includes = %s, deps = %s)" % (repr(headers), repr(includes), repr(deps)))
    lines.append('filegroup(name = "archives", srcs = %s)' % repr(archives))
    ctx.file("BUILD.bazel", "\n".join(lines) + "\n")

_alipay_sdk = repository_rule(
    implementation = _sdk_impl,
    attrs = {"manifest": attr.label(default = Label("//libs/alipay:sdk_manifest.json"))},
    environ = ["ALIPAY_SDK_PATH"],
    local = True,
)

def _extension_impl(module_ctx):
    _alipay_sdk(name = "alipay_sdk")

alipay_sdk = module_extension(implementation = _extension_impl)
