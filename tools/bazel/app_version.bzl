"""Shared App versions consumed by firmware, Lua packages, and other artifacts.

Apps declare their shared base version independently of boards. Firmware
targets combine it with a target variant and the board's integer build counter.
"""

load(
    ":firmware.bzl",
    _AppVersionDeclarationInfo = "AppVersionDeclarationInfo",
    _AppVersionInfo = "FirmwareVersionInfo",
    _validate_app_version = "validate_declared_version",
)

AppVersionInfo = _AppVersionInfo
validate_app_version = _validate_app_version

def _app_version_impl(ctx):
    validate_app_version(ctx.attr.value)
    return [AppVersionInfo(value = ctx.attr.value), _AppVersionDeclarationInfo()]

app_version = rule(
    implementation = _app_version_impl,
    attrs = {"value": attr.string(mandatory = True)},
    doc = "Declares the shared version of one App, without firmware-specific configuration.",
)
