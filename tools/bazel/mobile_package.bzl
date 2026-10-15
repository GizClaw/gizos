"""Local native SDK packaging and imports; no publishing side effects."""

load("@rules_cc//cc:defs.bzl", "cc_import", "cc_library")

def _package_impl(ctx):
    artifact = ctx.actions.declare_file(ctx.label.name + (".xcframework.zip" if ctx.attr.kind == "swift" else ".aar"))
    bundle = ctx.actions.declare_directory(ctx.label.name + ".package")
    args = ctx.actions.args()
    args.add_all([
        "package",
        "--kind",
        ctx.attr.kind,
        "--binary",
        ctx.file.binary.path,
        "--name",
        ctx.attr.module_name,
        "--version",
        ctx.attr.version,
        "--artifact",
        artifact.path,
        "--bundle",
        bundle.path,
    ])
    for header in ctx.files.headers:
        args.add_all(["--header", header.path])
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [args],
        inputs = [ctx.file.binary] + ctx.files.headers,
        outputs = [artifact, bundle],
        mnemonic = "MobileSDKPackage",
    )
    return [
        DefaultInfo(files = depset([artifact, bundle])),
        OutputGroupInfo(artifact = depset([artifact]), package = depset([bundle])),
    ]

_mobile_package = rule(
    implementation = _package_impl,
    attrs = {
        "kind": attr.string(mandatory = True, values = ["swift", "android"]),
        "binary": attr.label(allow_single_file = True, mandatory = True),
        "headers": attr.label_list(allow_files = [".h"]),
        "module_name": attr.string(mandatory = True),
        "version": attr.string(mandatory = True),
        "_tool": attr.label(default = "//tools/bazel:mobile_package_tool", executable = True, cfg = "exec"),
    },
)

def h2_swift_package(name, xcframework, headers, module_name, version, **kwargs):
    """Wrap an apple_static_xcframework as a local Swift package + checksum."""
    _mobile_package(
        name = name,
        kind = "swift",
        binary = xcframework,
        headers = headers,
        module_name = module_name,
        version = version,
        **kwargs
    )

def h2_android_aar(name, shared_library, headers, module_name, version, **kwargs):
    """Package an arm64 Android shared library with Prefab headers and a POM."""
    _mobile_package(
        name = name,
        kind = "android",
        binary = shared_library,
        headers = headers,
        module_name = module_name,
        version = version,
        **kwargs
    )

def _extract_impl(ctx):
    headers = ctx.actions.declare_directory(ctx.label.name + "/Headers")
    binary = ctx.actions.declare_file(ctx.label.name + "/" + ctx.attr.binary_name)
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [
            "extract",
            "--archive",
            ctx.file.archive.path,
            "--member",
            ctx.attr.member,
            "--header-prefix",
            ctx.attr.header_prefix,
            "--binary",
            binary.path,
            "--headers",
            headers.path,
        ],
        inputs = [ctx.file.archive],
        outputs = [binary, headers],
        mnemonic = "MobileSDKImport",
    )
    return [
        DefaultInfo(files = depset([binary, headers])),
        OutputGroupInfo(binary = depset([binary]), headers = depset([headers])),
    ]

_extract = rule(
    implementation = _extract_impl,
    attrs = {
        "archive": attr.label(allow_single_file = True, mandatory = True),
        "member": attr.string(mandatory = True),
        "header_prefix": attr.string(mandatory = True),
        "binary_name": attr.string(mandatory = True),
        "_tool": attr.label(default = "//tools/bazel:mobile_package_tool", executable = True, cfg = "exec"),
    },
)

def h2_mobile_archive_import(name, archive, member, header_prefix, binary_name, shared = False, linkopts = [], **kwargs):
    """Compile against headers and link against bytes extracted from the SDK."""
    _extract(
        name = name + "_files",
        archive = archive,
        member = member,
        header_prefix = header_prefix,
        binary_name = binary_name,
        **kwargs
    )
    native.filegroup(name = name + "_binary", srcs = [":" + name + "_files"], output_group = "binary")
    native.filegroup(name = name + "_headers", srcs = [":" + name + "_files"], output_group = "headers")
    library = {"shared_library" if shared else "static_library": ":" + name + "_binary"}
    library.update(kwargs)
    cc_import(name = name + "_link", **library)
    cc_library(
        name = name,
        hdrs = [":" + name + "_headers"],
        includes = [name + "_files/Headers"],
        deps = [":" + name + "_link"],
        linkopts = linkopts,
        **kwargs
    )
