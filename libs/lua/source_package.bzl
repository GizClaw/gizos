"""Export the configured portable C closure, without compiling Bazel archives."""

load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

_CSourcesInfo = provider(
    doc = "Portable sources and compile contract collected from the configured C graph.",
    fields = ["files", "units", "includes", "defines"],
)

def _path(path):
    # File.short_path uses ../<canonical repository>/ for external files.
    return "external/" + path[3:] if path.startswith("../") else path

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
            includes.append("/".join([p for p in [root, ctx.label.package, inc] if p]))
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
    spec = ctx.actions.declare_file(ctx.label.name + ".json")
    ctx.actions.write(spec, json.encode({
        "schema_version": 1,
        "gizos_commit": "@GIZOS_COMMIT@",
        "runtime_profile_id": "runtime.lua.gizos",
        "sources": sorted([_path(f.short_path) for f in files if f.extension == "c"]),
        "include_dirs": sorted(closure.includes.to_list()),
        "defines": sorted(closure.defines.to_list()),
        "cflags": [],
        "compilation_units": [json.decode(u) for u in closure.units.to_list()],
        "per_os": {os: {"link_flags": ["-lm"]} for os in ["linux", "darwin", "android", "ios"]},
        "files": dict({_path(f.short_path): f.path for f in files}, LICENSE = ctx.file.license.path),
    }))
    archive = ctx.actions.declare_file("gizos-lua-runtime-src.tar.gz")
    ctx.actions.run(
        executable = ctx.executable._pack,
        arguments = [spec.path, archive.path],
        inputs = depset([spec, ctx.file.license], transitive = [closure.files]),
        outputs = [archive],
        mnemonic = "LuaSourcePackage",
    )
    return [DefaultInfo(files = depset([archive]))]

lua_source_package = rule(
    implementation = _package_impl,
    attrs = {
        "license": attr.label(default = "//:LICENSE", allow_single_file = True),
        "defaults": attr.label(default = "//libs/pal:unsupported", aspects = [_sources]),
        "runtime": attr.label(mandatory = True, aspects = [_sources]),
        "_pack": attr.label(default = ":pack_sources", executable = True, cfg = "exec"),
    },
)
