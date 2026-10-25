"""Manual board build numbers, independent of BSP source files."""

BoardVersionInfo = provider(
    doc = "Build number declared by one board.",
    fields = {"build": "Non-negative integer maintained in the board's package."},
)

def _board_version_impl(ctx):
    if ctx.attr.build < 0:
        fail("board build must be non-negative")
    return [BoardVersionInfo(build = ctx.attr.build)]

board_version = rule(
    implementation = _board_version_impl,
    attrs = {"build": attr.int(mandatory = True)},
    doc = "Declares a manual board build number without source dependencies.",
)
