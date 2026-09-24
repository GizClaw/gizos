"""Export the configured portable C closure, without compiling Bazel archives."""

load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")
load("@rules_pkg//pkg:providers.bzl", "PackageFilesInfo")

_CSourcesInfo = provider(
    doc = "Portable sources and compile contract collected from the configured C graph.",
    fields = ["files", "units", "includes", "defines"],
)

def _path(path):
    # Match the apparent vendor identity, never publish canonical repo names.
    if path.startswith("../") or path.startswith("external/"):
        relative = path[3:] if path.startswith("../") else path[len("external/"):]
        parts = relative.split("/")
        repository = parts[0].split("+")[-1]
        vendors = {
            "h2_vendor_lua": "lua",
            "h2_vendor_tlsf": "tlsf",
            "h2_vendor_yyjson": "yyjson",
        }
        if repository not in vendors:
            fail("Unknown portable source repository: %s" % repository)
        return "/".join(["third_party", vendors[repository]] + parts[1:])
    return path

def _sources_impl(target, ctx):
    package = ctx.label.package
    if package.startswith("libs/pal/providers/") or package == "libs/bleikcp" or package.startswith("boards/") or package.startswith("native_component_src/"):
        fail("Platform assembly leaked into portable Lua: %s" % ctx.label)
    if getattr(ctx.rule.attr, "implementation_deps", []):
        fail("Portable source export requires explicit deps: %s" % ctx.label)
    if getattr(ctx.rule.attr, "linkopts", []):
        fail("Declare new platform link requirements in the source manifest: %s" % ctx.label)
    children = [d[_CSourcesInfo] for d in getattr(ctx.rule.attr, "deps", []) if _CSourcesInfo in d]
    files = []
    sources = []
    for name in ["srcs", "hdrs", "textual_hdrs"]:
        for dep in getattr(ctx.rule.attr, name, []):
            for f in dep[DefaultInfo].files.to_list():
                if f.extension not in ["c", "h"]:
                    fail("Non-C input in portable closure: %s" % f.path)
                files.append(f)
                if f.extension == "c" and name == "srcs":
                    sources.append(_path(f.short_path))
    includes = []
    defines = []
    if CcInfo in target:
        compilation = target[CcInfo].compilation_context
        defines = compilation.defines.to_list()

        # Includes are derived from the owning rule, avoiding execroot paths.
        root = ctx.label.workspace_root
        for inc in getattr(ctx.rule.attr, "includes", []):
            includes.append(_path("/".join([p for p in [root, ctx.label.package, inc] if p])))
    units = []
    if sources:
        units.append(json.encode({
            "sources": sources,
            "cflags": getattr(ctx.rule.attr, "copts", []) + getattr(ctx.rule.attr, "conlyopts", []),
            "defines": getattr(ctx.rule.attr, "local_defines", []),
        }))
    return [_CSourcesInfo(
        files = depset(files, transitive = [c.files for c in children]),
        units = depset(units, transitive = [c.units for c in children]),
        includes = depset(includes, transitive = [c.includes for c in children]),
        defines = depset(defines, transitive = [c.defines for c in children]),
    )]

_sources = aspect(implementation = _sources_impl, attr_aspects = ["deps"])

def _package_impl(ctx):
    roots = [ctx.attr.runtime[_CSourcesInfo], ctx.attr.defaults[_CSourcesInfo]]
    closure = _CSourcesInfo(**{field: depset(transitive = [getattr(r, field) for r in roots]) for field in ["files", "units", "includes", "defines"]})
    files = closure.files.to_list()
    for f in files:
        if "/providers/" in f.short_path or f.short_path.startswith("boards/") or "/bleikcp/" in f.short_path:
            fail("Platform assembly leaked into portable Lua: %s" % f.short_path)
    manifest = ctx.actions.declare_file(ctx.label.name + "/manifest.json")
    ctx.actions.write(manifest, json.encode_indent({
        "schema_version": 1,
        "runtime_profile_id": "runtime.lua.gizos",
        "sources": sorted([_path(f.short_path) for f in files if f.extension == "c"]),
        "include_dirs": sorted(closure.includes.to_list()),
        "defines": sorted(closure.defines.to_list()),
        "cflags": [],
        "compilation_units": [json.decode(u) for u in sorted(closure.units.to_list())],
        "per_os": {os: {"link_flags": ["-lm"]} for os in ["linux", "darwin", "android", "ios"]},
    }, indent = "  ") + "\n")
    dest_src_map = {_path(f.short_path): f for f in files}
    dest_src_map["LICENSE"] = ctx.file.license
    dest_src_map["manifest.json"] = manifest
    content_id = ctx.actions.declare_file("runtime_sources.content_id")
    listing = ctx.actions.declare_file(ctx.label.name + ".inputs.json")
    ctx.actions.write(listing, json.encode({path: f.path for path, f in dest_src_map.items()}))
    ctx.actions.run(
        executable = ctx.executable._content_id_tool,
        arguments = [listing.path, content_id.path],
        inputs = depset([listing] + dest_src_map.values()),
        outputs = [content_id],
        mnemonic = "LuaSourceContentId",
    )
    return [
        DefaultInfo(files = depset([manifest, ctx.file.license, content_id], transitive = [closure.files])),
        OutputGroupInfo(content_id = depset([content_id])),
        PackageFilesInfo(dest_src_map = dest_src_map, attributes = {"mode": "0644"}),
    ]

lua_source_package = rule(
    implementation = _package_impl,
    attrs = {
        "_content_id_tool": attr.label(default = ":source_content_id", executable = True, cfg = "exec"),
        "license": attr.label(default = "//:LICENSE", allow_single_file = True),
        "defaults": attr.label(default = "//libs/pal:unsupported", aspects = [_sources]),
        "runtime": attr.label(mandatory = True, aspects = [_sources]),
    },
)
