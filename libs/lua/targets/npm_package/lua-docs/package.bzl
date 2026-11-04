"""Static Runtime documentation assembly, with explicit canonical inputs."""

def _package_impl(ctx):
    output = ctx.actions.declare_directory(ctx.label.name)
    manifest = ctx.actions.declare_file(ctx.label.name + ".package.json")
    listing = ctx.actions.declare_file(ctx.label.name + ".inputs.json")
    inputs = ctx.files.srcs
    mapping = {}
    for source in inputs:
        name = source.short_path
        if name.startswith("../"):
            name = "/".join(name.split("/")[2:])
        if name in mapping:
            fail("Duplicate documentation input: %s" % name)
        mapping[name] = source.path
    ctx.actions.write(listing, json.encode(mapping))
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [
            "build",
            "--inputs",
            listing.path,
            "--output",
            output.path,
            "--package-json-output",
            manifest.path,
        ],
        inputs = depset([listing] + inputs),
        outputs = [output, manifest],
        mnemonic = "LuaDocsPackage",
        progress_message = "Assembling versioned Lua Runtime documentation %{label}",
    )
    return [
        DefaultInfo(files = depset([output])),
        OutputGroupInfo(package_json = depset([manifest])),
    ]

lua_docs_package = rule(
    implementation = _package_impl,
    attrs = {
        "srcs": attr.label_list(allow_files = True, mandatory = True),
        "_tool": attr.label(default = ":assembler", cfg = "exec", executable = True),
    },
)
