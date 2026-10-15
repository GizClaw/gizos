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
        "installed_entry": "The exact raw or compact text placed in the archive.",
        "data": "Original payload files below the sibling app data directory.",
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
    installed_entry = ctx.actions.declare_file(ctx.label.name + ".installed/" + entry.basename)
    specification = ctx.actions.declare_file(ctx.label.name + ".inputs.json")
    ctx.actions.write(specification, json.encode({
        "app_id": app_id,
        "version": version,
        "entry": entry.basename,
        "compact": ctx.attr.compact,
        "display_name": ctx.attr.display_name,
        "description": ctx.attr.description,
        "files": files,
    }))
    args = ctx.actions.args()
    args.add("--specification", specification)
    args.add("--archive", archive)
    args.add("--manifest", manifest)
    args.add("--metadata", metadata)
    args.add("--installed-entry", installed_entry)
    ctx.actions.run(
        executable = ctx.executable._packager,
        arguments = [args],
        inputs = depset([specification, entry] + ctx.files.data),
        outputs = [archive, manifest, metadata, installed_entry],
        mnemonic = "LuaAppPackage",
        progress_message = "Packaging Lua app %{label}",
    )
    return [
        DefaultInfo(files = depset([archive, manifest, metadata])),
        OutputGroupInfo(
            archive = depset([archive]),
            manifest = depset([manifest]),
            metadata = depset([metadata]),
            installed_files = depset([installed_entry] + ctx.files.data),
            installed_entry = depset([installed_entry]),
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
            installed_entry = installed_entry,
            data = ctx.files.data,
        ),
    ]

lua_app = rule(
    implementation = _lua_app_impl,
    attrs = {
        "entry": attr.label(allow_single_file = [".lua"], mandatory = True, doc = "One text Lua entry; its filename determines the stable app id."),
        "data": attr.label_list(allow_files = True, doc = "Explicit optional files below the entry's same-name sibling directory."),
        "display_name": attr.string(default = "", doc = "Optional UTF-8 catalog name, at most 128 bytes."),
        "description": attr.string(default = "", doc = "Optional UTF-8 description, at most 1024 bytes."),
        "compact": attr.bool(default = False, doc = "Use the shared Lua source compactor, preserving literal bytes and source lines."),
        "version": attr.label(mandatory = True, providers = [FirmwareVersionInfo], doc = "An app-owned firmware_version target; no implicit batch or workspace version."),
        "_packager": attr.label(default = Label("//libs/lua:app_package"), executable = True, cfg = "exec"),
    },
    doc = "Packages a portable Lua application without building or activating firmware.",
)

def _lua_app_index_impl(ctx):
    out = ctx.actions.declare_file(ctx.label.name + ".json")
    spec = ctx.actions.declare_file(ctx.label.name + ".inputs.json")
    manifests = [target[LuaAppInfo].manifest for target in ctx.attr.apps]
    ctx.actions.write(spec, json.encode({"root": ctx.attr.root, "manifests": [file.path for file in manifests]}))
    args = ctx.actions.args()
    args.add("--specification", spec)
    args.add("--output", out)
    ctx.actions.run(
        executable = ctx.executable._indexer,
        arguments = [args],
        inputs = depset([spec] + manifests),
        outputs = [out],
        mnemonic = "LuaAppIndex",
    )
    installed = []
    for target in ctx.attr.apps:
        info = target[LuaAppInfo]
        installed.append(info.installed_entry)
        installed.extend(info.data)
    return [DefaultInfo(files = depset([out])), OutputGroupInfo(installed_files = depset(installed))]

lua_app_index = rule(
    implementation = _lua_app_index_impl,
    attrs = {
        "apps": attr.label_list(mandatory = True, providers = [LuaAppInfo]),
        "root": attr.string(mandatory = True, doc = "Installed filesystem root containing the entries and same-name data directories."),
        "_indexer": attr.label(default = Label("//libs/lua:app_index"), executable = True, cfg = "exec"),
    },
    doc = "Generates a preinstalled catalog from versioned app Providers, with no second version table.",
)
