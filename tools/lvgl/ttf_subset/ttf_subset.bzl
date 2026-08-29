"""Public rule for embedding a deterministic TTF subset as C data."""

_C_IDENTIFIER_FIRST = "_ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
_C_IDENTIFIER_REST = _C_IDENTIFIER_FIRST + "0123456789"

def _valid_identifier(value):
    if not value or value[0] not in _C_IDENTIFIER_FIRST:
        return False
    for index in range(1, len(value)):
        if value[index] not in _C_IDENTIFIER_REST:
            return False
    return True

def _ttf_subset_c_impl(ctx):
    if not _valid_identifier(ctx.attr.symbol):
        fail("symbol must be a valid C identifier")
    if not ctx.files.symbol_sources and not ctx.attr.ranges:
        fail("at least one symbol_sources file or range is required")
    for value in ctx.attr.ranges:
        if not value:
            fail("ranges cannot contain an empty value")
    if not ctx.outputs.out_c.basename.endswith(".c"):
        fail("out_c must name a C source")
    if not ctx.outputs.out_h.basename.endswith(".h"):
        fail("out_h must name a C header")

    arguments = ctx.actions.args()
    arguments.add("--font", ctx.file.font.path)
    arguments.add("--symbol", ctx.attr.symbol)
    arguments.add("--out-c", ctx.outputs.out_c.path)
    arguments.add("--out-h", ctx.outputs.out_h.path)
    for source in ctx.files.symbol_sources:
        arguments.add("--symbol-source", source.path)
    for value in ctx.attr.ranges:
        arguments.add("--range", value)

    ctx.actions.run(
        arguments = [arguments],
        executable = ctx.executable._runner,
        inputs = depset([ctx.file.font] + ctx.files.symbol_sources),
        mnemonic = "TtfSubsetC",
        outputs = [ctx.outputs.out_c, ctx.outputs.out_h],
        progress_message = "Generating embedded TTF subset %{label}",
        tools = [ctx.attr._runner[DefaultInfo].files_to_run],
    )
    return [DefaultInfo(files = depset([ctx.outputs.out_c, ctx.outputs.out_h]))]

ttf_subset_c = rule(
    implementation = _ttf_subset_c_impl,
    attrs = {
        "font": attr.label(
            allow_single_file = [".ttf"],
            mandatory = True,
            doc = "Consumer-owned source font.",
        ),
        "out_c": attr.output(mandatory = True),
        "out_h": attr.output(mandatory = True),
        "ranges": attr.string_list(
            doc = "Inclusive Unicode ranges such as 0x20-0x7E.",
        ),
        "symbol": attr.string(
            mandatory = True,
            doc = "C prefix exporting <symbol>_data and <symbol>_size.",
        ),
        "symbol_sources": attr.label_list(
            allow_files = True,
            doc = "UTF-8 files whose unique non-control characters are retained.",
        ),
        "_runner": attr.label(
            cfg = "exec",
            default = Label("@gizos//tools/lvgl/ttf_subset:runner"),
            executable = True,
        ),
    },
    doc = "Subsets one TTF and emits its font bytes as portable C source/header.",
)
