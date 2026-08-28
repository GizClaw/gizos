"""Public rule for generating a fixed-size LVGL C font."""

load("@aspect_rules_js//js:providers.bzl", "JsInfo")

_C_IDENTIFIER_FIRST = "_abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
_C_IDENTIFIER_REST = _C_IDENTIFIER_FIRST + "0123456789"
_SUPPORTED_BPP = [1, 2, 3, 4, 8]

def _node_runtime_tool_impl(ctx):
    node = ctx.toolchains["@rules_nodejs//nodejs:runtime_toolchain_type"].nodeinfo.node
    if not node:
        fail("the LVGL font converter requires a hermetic Node executable")
    launcher = ctx.actions.declare_file(ctx.label.name + ".exe")
    ctx.actions.symlink(
        output = launcher,
        target_file = node,
        is_executable = True,
    )
    return [DefaultInfo(
        executable = launcher,
        files = depset([launcher]),
        runfiles = ctx.runfiles(files = [node]),
    )]

node_runtime_tool = rule(
    implementation = _node_runtime_tool_impl,
    executable = True,
    toolchains = ["@rules_nodejs//nodejs:runtime_toolchain_type"],
)

def _valid_identifier(value):
    if not value or value[0] not in _C_IDENTIFIER_FIRST:
        return False
    for index in range(1, len(value)):
        if value[index] not in _C_IDENTIFIER_REST:
            return False
    return True

def _lvgl_font_impl(ctx):
    if ctx.attr.size <= 0:
        fail("size must be positive")
    if ctx.attr.bpp not in _SUPPORTED_BPP:
        fail("bpp must be one of %s" % _SUPPORTED_BPP)
    if not _valid_identifier(ctx.attr.font_name):
        fail("font_name must be a valid C identifier")
    if not ctx.files.symbol_sources and not ctx.attr.ranges:
        fail("at least one symbol_sources file or range is required")
    for value in ctx.attr.ranges:
        if not value:
            fail("ranges cannot contain an empty value")
    if not ctx.outputs.out.basename.endswith(".c"):
        fail("out must name a C source")

    package_stores = ctx.attr._converter_package[JsInfo].npm_package_store_infos.to_list()
    if len(package_stores) != 1:
        fail("lv_font_conv package must provide exactly one npm package store")
    converter_package = package_stores[0].package_store_directory

    arguments = ctx.actions.args()
    arguments.add("--node", ctx.executable._node.path)
    arguments.add("--converter-entry", ctx.file._converter_entry.path)
    arguments.add("--converter-package", converter_package.path)
    arguments.add("--font", ctx.file.font.path)
    for source in ctx.files.symbol_sources:
        arguments.add("--symbol-source", source.path)
    for value in ctx.attr.ranges:
        arguments.add("--range", value)
    arguments.add("--size", ctx.attr.size)
    arguments.add("--bpp", ctx.attr.bpp)
    arguments.add("--font-name", ctx.attr.font_name)
    arguments.add("--lv-include", ctx.attr.lv_include)
    arguments.add("--output", ctx.outputs.out.path)

    tool_files = [ctx.file._converter_entry]
    converter_tools = depset(
        direct = tool_files,
        transitive = [ctx.attr._converter_package[DefaultInfo].files],
    )

    ctx.actions.run(
        arguments = [arguments],
        executable = ctx.executable._runner,
        inputs = depset([ctx.file.font] + ctx.files.symbol_sources),
        mnemonic = "LvglFontConv",
        outputs = [ctx.outputs.out],
        progress_message = "Generating LVGL font %{label}",
        env = {"BAZEL_BINDIR": ctx.bin_dir.path},
        tools = [
            ctx.attr._node[DefaultInfo].files_to_run,
            converter_tools,
        ],
    )
    return [DefaultInfo(files = depset([ctx.outputs.out]))]

lvgl_font = rule(
    implementation = _lvgl_font_impl,
    attrs = {
        "bpp": attr.int(
            default = 4,
            doc = "Bits per pixel used for glyph anti-aliasing.",
        ),
        "font": attr.label(
            allow_single_file = [".otf", ".ttf", ".woff", ".woff2"],
            mandatory = True,
            doc = "Source font owned by the consumer.",
        ),
        "font_name": attr.string(
            mandatory = True,
            doc = "C identifier exported as a const lv_font_t.",
        ),
        "lv_include": attr.string(
            default = "include/lvgl/lvgl.h",
            doc = "LVGL include path emitted by the official converter.",
        ),
        "out": attr.output(
            mandatory = True,
            doc = "Generated LVGL C source.",
        ),
        "ranges": attr.string_list(
            doc = "Additional lv_font_conv Unicode ranges.",
        ),
        "size": attr.int(
            mandatory = True,
            doc = "Single generated pixel size.",
        ),
        "symbol_sources": attr.label_list(
            allow_files = True,
            doc = "UTF-8 files whose unique non-control characters form the symbol set.",
        ),
        "_converter_entry": attr.label(
            cfg = "exec",
            allow_single_file = [".cjs"],
            default = Label("@gizos//tools/lvgl/font_conv:font_conv_entry.cjs"),
        ),
        "_converter_package": attr.label(
            cfg = "exec",
            default = Label("@gizos//tools/lvgl/font_conv:node_modules/lv_font_conv"),
            providers = [JsInfo],
        ),
        "_node": attr.label(
            cfg = "exec",
            default = Label("@gizos//tools/lvgl/font_conv:node_runtime"),
            executable = True,
        ),
        "_runner": attr.label(
            cfg = "exec",
            default = Label("@gizos//tools/lvgl/font_conv:font_conv_runner"),
            executable = True,
        ),
    },
    doc = "Generates one static LVGL C font with the pinned official converter.",
)
