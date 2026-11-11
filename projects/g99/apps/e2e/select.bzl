"""Case selection that a G99 E2E device package carries.

A device cannot take the selection from its environment, so the package
writes `--//projects/g99/apps/e2e:select` into its data, where the device
launcher reads it at boot. Changing it repacks the data only; the firmware
does not depend on the flag and stays cached.
"""

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")

def _g99_e2e_select_file_impl(ctx):
    output = ctx.actions.declare_file(ctx.attr.out)
    ctx.actions.write(output, ctx.attr._select[BuildSettingInfo].value + "\n")
    return [DefaultInfo(files = depset([output]))]

g99_e2e_select_file = rule(
    implementation = _g99_e2e_select_file_impl,
    doc = "Writes the selected cases to `out`, relative to this package.",
    attrs = {
        "out": attr.string(mandatory = True),
        "_select": attr.label(default = "//projects/g99/apps/e2e:select"),
    },
)
