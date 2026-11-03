"""Generate the Bluetooth privacy metadata embedded in native loader executables."""

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")

def _cli_info_plist_impl(ctx):
    description = ctx.attr.description[BuildSettingInfo].value
    if not description.strip():
        fail("%s must contain a Bluetooth usage description" % ctx.attr.description.label)
    escaped = description.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;").replace("'", "&apos;")
    ctx.actions.write(ctx.outputs.out, """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleIdentifier</key>
    <string>%s</string>
    <key>CFBundleName</key>
    <string>Gizloader</string>
    <key>CFBundleExecutable</key>
    <string>%s</string>
    <key>CFBundleVersion</key>
    <string>1</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>NSBluetoothAlwaysUsageDescription</key>
    <string>%s</string>
    <key>NSBluetoothPeripheralUsageDescription</key>
    <string>%s</string>
</dict>
</plist>
""" % (ctx.attr.bundle_id, ctx.attr.executable, escaped, escaped))
    return [DefaultInfo(files = depset([ctx.outputs.out]))]

cli_info_plist = rule(
    implementation = _cli_info_plist_impl,
    attrs = {
        "bundle_id": attr.string(default = "com.gizclaw.gizloader.cli"),
        "description": attr.label(mandatory = True, providers = [BuildSettingInfo]),
        "executable": attr.string(default = "gizloader-cli"),
        "out": attr.output(mandatory = True),
    },
)
