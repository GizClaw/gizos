"""Versioned Lua application packages: one entry and optional sibling data."""

load("//tools/bazel:firmware.bzl", "FirmwareVersionInfo", "validate_declared_version")

LuaAppInfo = provider(
    doc = "Portable Lua app identity and its verified release outputs.",
    fields = {
        "app_id": "Entry basename without .lua; also the job storage identity.",
        "version": "Explicit declared SemVer, independent of the release batch.",
        "entry": "Package-relative Lua entry filename.",
        "data_dir": "Optional package-relative sibling data directory, or None.",
        "compact": "Whether the shared Lua source compactor is selected.",
        "archive": "Deterministic .lua-app.tar.zlib package.",
        "manifest": "Package manifest with file lengths and SHA-256 values.",
        "metadata": "Release metadata with package length and SHA-256.",
    },
)

def _lua_app_impl(ctx):
    entry = ctx.file.entry
    app_id = entry.basename[:-4]
    if not app_id or len(app_id) > 32 or app_id.startswith("."):
        fail("Lua app entry must have a valid 1..32 byte app id before .lua")
    for character in app_id.elems():
        if character not in "abcdefghijklmnopqrstuvwxyz0123456789_.-":
            fail("Lua app id contains an invalid character: " + character)
    version = ctx.attr.version[FirmwareVersionInfo].value
    validate_declared_version(version)
    prefix = entry.short_path[:-len(entry.basename)]
    data_prefix = prefix + app_id + "/"
    files = {entry.basename: entry.path}
    for source in ctx.files.data:
        if source.is_directory or not source.short_path.startswith(data_prefix):
            fail("Lua app data must be files below the entry's sibling %s/ directory: %s" % (app_id, source.short_path))
        path = source.short_path[len(prefix):]
        if path in files:
            fail("Duplicate Lua app package path: " + path)
        files[path] = source.path
    stem = app_id + "-" + version + ".lua-app"
    archive = ctx.actions.declare_file(stem + ".tar.zlib")
    manifest = ctx.actions.declare_file(stem + ".manifest.json")
    metadata = ctx.actions.declare_file(stem + ".json")
    specification = ctx.actions.declare_file(ctx.label.name + ".inputs.json")
    ctx.actions.write(specification, json.encode({
        "app_id": app_id,
        "version": version,
        "entry": entry.basename,
        "compact": ctx.attr.compact,
        "files": files,
    }))
    args = ctx.actions.args()
    args.add("--specification", specification)
    args.add("--archive", archive)
    args.add("--manifest", manifest)
    args.add("--metadata", metadata)
    ctx.actions.run(
        executable = ctx.executable._packager,
        arguments = [args],
        inputs = depset([specification, entry] + ctx.files.data),
        outputs = [archive, manifest, metadata],
        mnemonic = "LuaAppPackage",
        progress_message = "Packaging Lua app %{label}",
    )
    return [
        DefaultInfo(files = depset([archive, manifest, metadata])),
        OutputGroupInfo(
            archive = depset([archive]),
            manifest = depset([manifest]),
            metadata = depset([metadata]),
        ),
        LuaAppInfo(
            app_id = app_id,
            version = version,
            entry = entry.basename,
            data_dir = app_id if ctx.files.data else None,
            compact = ctx.attr.compact,
            archive = archive,
            manifest = manifest,
            metadata = metadata,
        ),
    ]

lua_app = rule(
    implementation = _lua_app_impl,
    attrs = {
        "entry": attr.label(allow_single_file = [".lua"], mandatory = True, doc = "One text Lua entry; its filename determines the stable app id."),
        "data": attr.label_list(allow_files = True, doc = "Explicit optional files below the entry's same-name sibling directory."),
        "compact": attr.bool(default = False, doc = "Use the shared Lua source compactor, preserving literal bytes and source lines."),
        "version": attr.label(mandatory = True, providers = [FirmwareVersionInfo], doc = "An app-owned firmware_version target; no implicit batch or workspace version."),
        "_packager": attr.label(default = Label("//libs/lua:app_package"), executable = True, cfg = "exec"),
    },
    doc = "Packages a portable Lua application without building or activating firmware.",
)
