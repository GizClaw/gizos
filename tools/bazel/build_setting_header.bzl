"""Generate a C unsigned decimal constant from a string build setting."""

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")

def _build_setting_header_impl(ctx):
    value = ctx.attr.flag[BuildSettingInfo].value
    if not value or any([c not in "0123456789" for c in value.elems()]):
        fail("%s must contain decimal digits only, got %r" % (ctx.attr.flag.label, value))
    guard = ctx.attr.macro + "_HEADER_INCLUDED"
    ctx.actions.write(ctx.outputs.out, "#ifndef %s\n#define %s\n#define %s %su\n#endif\n" % (
        guard,
        guard,
        ctx.attr.macro,
        value,
    ))
    return [DefaultInfo(files = depset([ctx.outputs.out]))]

build_setting_header = rule(
    implementation = _build_setting_header_impl,
    attrs = {
        "flag": attr.label(mandatory = True, providers = [BuildSettingInfo]),
        "macro": attr.string(mandatory = True),
        "out": attr.output(mandatory = True),
    },
)
