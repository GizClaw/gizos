"""Firmware versions composed from an App, a target variant, and a board."""

load(":board_version.bzl", "BoardVersionInfo")

# Keep the public AppVersionInfo/FirmwareVersionInfo provider alias compatible,
# while requiring a real App declaration for the composed app_version input.
AppVersionDeclarationInfo = provider(
    doc = "Marks a version declared through app_version; aliases may forward it.",
    fields = [],
)

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

def validate_declared_version(value):
    """Validates the shared 31-byte SemVer contract for declared artifacts."""
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
    if ctx.attr.main_version or ctx.attr.build:
        if ctx.attr.value:
            fail("firmware_version must use either main_version/build or a legacy value, not both")
        if not ctx.attr.main_version or not ctx.attr.build:
            fail("firmware_version requires both main_version and build")
        main_version = ctx.attr.main_version[FirmwareVersionInfo].value
        validate_declared_version(main_version)
        if "-" in main_version or "+" in main_version:
            fail("main_version must be MAJOR.MINOR.PATCH without a suffix when composing a firmware version")
        for character in ctx.attr.variant.elems():
            if character not in _SEMVER_IDENTIFIER_CHARACTERS:
                fail("firmware version variant must contain only ASCII letters, digits, and hyphens")

        # Separate the variant from the numeric build so standard SemVer
        # compares build counters numerically across digit boundaries.
        suffix = "%s.%d" % (ctx.attr.variant, ctx.attr.build[BoardVersionInfo].build) if ctx.attr.variant else str(ctx.attr.build[BoardVersionInfo].build)
        value = "%s-%s" % (main_version, suffix)
        if ctx.attr.build_metadata:
            value += "+" + ctx.attr.build_metadata
    else:
        if ctx.attr.variant:
            fail("firmware version variant requires main_version and build")
        if ctx.attr.build_metadata:
            fail("firmware version build_metadata requires main_version and build")
        if not ctx.attr.value:
            fail("firmware_version requires main_version and build, or a legacy value")
        value = ctx.attr.value
    validate_declared_version(value)
    return [FirmwareVersionInfo(value = value)]

def _firmware_version_flag_impl(ctx):
    value = ctx.build_setting_value
    _validate_safe(value)
    return [FirmwareVersionInfo(value = value)]

firmware_version = rule(
    implementation = _firmware_version_impl,
    attrs = {
        "main_version": attr.label(
            providers = [FirmwareVersionInfo, AppVersionDeclarationInfo],
            doc = "Base MAJOR.MINOR.PATCH version declared by app_version, optionally forwarded through an alias.",
        ),
        "build": attr.label(
            providers = [BoardVersionInfo],
            doc = "Numeric build counter declared by board_version.",
        ),
        "variant": attr.string(
            default = "",
            doc = "Optional prerelease identifier; its business meaning belongs to the caller. The rule adds a dot before the numeric build.",
        ),
        "build_metadata": attr.string(
            default = "",
            doc = "Optional dot-separated SemVer build metadata, without the leading plus. It does not affect precedence.",
        ),
        "value": attr.string(
            doc = "Legacy complete version; cannot be combined with the composition attributes.",
        ),
    },
    doc = "Composes {main_version}-{variant}.{build}+{build_metadata}; empty optional fields omit their separators.",
)

firmware_version_flag = rule(
    implementation = _firmware_version_flag_impl,
    build_setting = config.string(flag = True),
)
