"""Hermetic build rule for compiling a text Lua source into a C resource."""

load("@rules_cc//cc:defs.bzl", "cc_library")

def _lua_resource_impl(ctx):
    source = ctx.file.src
    header = ctx.outputs.header
    implementation = ctx.outputs.implementation
    args = ctx.actions.args()
    args.add("--source", source)
    args.add("--header", header)
    args.add("--implementation", implementation)
    args.add("--symbol", ctx.attr.symbol)
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [args],
        inputs = [source],
        outputs = [header, implementation],
        mnemonic = "H2LuaResource",
        progress_message = "Embedding Lua resource %{label}",
    )
    return [DefaultInfo(files = depset([header, implementation]))]

_lua_resource = rule(
    implementation = _lua_resource_impl,
    attrs = {
        "src": attr.label(allow_single_file = [".lua"], mandatory = True),
        "symbol": attr.string(mandatory = True),
        "_tool": attr.label(
            default = Label("//libs/lua:embed_resource"),
            executable = True,
            cfg = "exec",
        ),
    },
    outputs = {
        "header": "%{name}.h",
        "implementation": "%{name}.c",
    },
)

def h2_lua_resource(name, src, symbol, visibility = None):
    """Creates a C library exposing `<symbol>` and `<symbol>_size`."""
    generated_name = name + "_generated"
    _lua_resource(
        name = generated_name,
        src = src,
        symbol = symbol,
    )
    cc_library(
        name = name,
        srcs = [":" + generated_name + ".c"],
        hdrs = [":" + generated_name + ".h"],
        strip_include_prefix = ".",
        visibility = visibility,
    )
