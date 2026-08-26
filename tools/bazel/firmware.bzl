"""Shared declarative and compatibility firmware version targets."""

FirmwareVersionInfo = provider(
    doc = "Validated firmware version selected by one final firmware target.",
    fields = {"value": "Validated firmware version."},
)

_SAFE_CHARACTERS = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz._-+"
_SEMVER_IDENTIFIER_CHARACTERS = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-"

def _validate_safe(value):
    if not value or len(value) > 95:
        fail("firmware version must contain 1..95 characters")
    for character in value.elems():
        if character not in _SAFE_CHARACTERS:
            fail("firmware version contains an unsafe character: %r" % character)

def _validate_numeric_identifier(value, field):
    if not value:
        fail("firmware version %s must not be empty" % field)
    if len(value) > 1 and value.startswith("0"):
        fail("firmware version %s must not contain a leading zero" % field)
    for character in value.elems():
        if character not in "0123456789":
            fail("firmware version %s must be numeric" % field)

def _validate_identifiers(value, field, reject_numeric_leading_zero):
    for identifier in value.split("."):
        if not identifier:
            fail("firmware version %s identifier must not be empty" % field)
        numeric = True
        for character in identifier.elems():
            if character not in _SEMVER_IDENTIFIER_CHARACTERS:
                fail("firmware version %s contains an unsafe character: %r" % (field, character))
            if character not in "0123456789":
                numeric = False
        if reject_numeric_leading_zero and numeric and len(identifier) > 1 and identifier.startswith("0"):
            fail("numeric firmware version %s identifiers must not contain a leading zero" % field)

def _validate_semver(value):
    if not value or len(value) > 31:
        fail("declared firmware version must contain 1..31 ASCII characters")
    parts = value.split("+")
    if len(parts) > 2:
        fail("declared firmware version contains multiple build metadata separators")
    core_and_prerelease = parts[0]
    if len(parts) == 2:
        _validate_identifiers(parts[1], "build metadata", False)
    parts = core_and_prerelease.split("-")
    core = parts[0]
    if len(parts) > 1:
        _validate_identifiers("-".join(parts[1:]), "prerelease", True)
    core_parts = core.split(".")
    if len(core_parts) != 3:
        fail("declared firmware version must use MAJOR.MINOR.PATCH SemVer")
    for field, identifier in zip(["major", "minor", "patch"], core_parts):
        _validate_numeric_identifier(identifier, field)

def _firmware_version_impl(ctx):
    value = ctx.attr.value
    _validate_semver(value)
    return [FirmwareVersionInfo(value = value)]

def _firmware_version_flag_impl(ctx):
    value = ctx.build_setting_value
    _validate_safe(value)
    return [FirmwareVersionInfo(value = value)]

firmware_version = rule(
    implementation = _firmware_version_impl,
    attrs = {
        "value": attr.string(mandatory = True),
    },
)

firmware_version_flag = rule(
    implementation = _firmware_version_flag_impl,
    build_setting = config.string(flag = True),
)
