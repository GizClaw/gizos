"""Generate a C unsigned decimal constant from a bounded string build setting."""

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")

def parse_bounded_setting(label, value, minimum, maximum):
    """Returns the canonical integer for a decimal build-setting override.

    Only a canonical decimal (digits only, no sign, no leading zero) inside
    [minimum, maximum] is accepted, so the emitted C literal can never be read
    as octal, exceed the consumer's counter, or reach zero. Anything else fails
    analysis with the offending label and value.
    """
    digits = "0123456789"
    if not value or any([c not in digits for c in value.elems()]):
        fail("%s must be a decimal integer in [%d, %d], got %r" % (label, minimum, maximum, value))
    number = int(value)
    if str(number) != value:
        fail("%s must be a canonical decimal without leading zeros, got %r" % (label, value))
    if number < minimum or number > maximum:
        fail("%s must be in [%d, %d], got %d" % (label, minimum, maximum, number))
    return number

def _build_setting_header_impl(ctx):
    if ctx.attr.minimum < 0 or ctx.attr.maximum < ctx.attr.minimum:
        fail("%s: minimum/maximum must satisfy 0 <= minimum <= maximum" % ctx.label)
    number = parse_bounded_setting(
        ctx.attr.flag.label,
        ctx.attr.flag[BuildSettingInfo].value,
        ctx.attr.minimum,
        ctx.attr.maximum,
    )
    guard = ctx.attr.macro + "_HEADER_INCLUDED"
    ctx.actions.write(ctx.outputs.out, "#ifndef %s\n#define %s\n#define %s %du\n#endif\n" % (
        guard,
        guard,
        ctx.attr.macro,
        number,
    ))
    return [DefaultInfo(files = depset([ctx.outputs.out]))]

build_setting_header = rule(
    implementation = _build_setting_header_impl,
    attrs = {
        "flag": attr.label(mandatory = True, providers = [BuildSettingInfo]),
        "macro": attr.string(mandatory = True),
        "minimum": attr.int(mandatory = True, doc = "Smallest accepted value, inclusive."),
        "maximum": attr.int(mandatory = True, doc = "Largest accepted value, inclusive."),
        "out": attr.output(mandatory = True),
    },
)
