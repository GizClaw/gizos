"""Shared App versions consumed by firmware, Lua packages, and other artifacts.

The generic names reuse the existing validated string rule and provider, so
artifact consumers and the legacy firmware_version entry point stay compatible.
"""

load(
    ":firmware.bzl",
    _AppVersionInfo = "FirmwareVersionInfo",
    _app_version = "firmware_version",
    _validate_app_version = "validate_declared_version",
)

AppVersionInfo = _AppVersionInfo
app_version = _app_version
validate_app_version = _validate_app_version
