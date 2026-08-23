"""Shared firmware version build setting."""

FirmwareVersionInfo = provider(fields = {"value": "Validated firmware version."})

def _firmware_version_impl(ctx):
    value = ctx.build_setting_value
    if not value or len(value) > 95:
        fail("firmware version must contain 1..95 characters")
    for character in value.elems():
        if character not in "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz._-":
            fail("firmware version contains an unsafe character: %r" % character)
    return [FirmwareVersionInfo(value = value)]

firmware_version = rule(
    implementation = _firmware_version_impl,
    build_setting = config.string(flag = True),
)
