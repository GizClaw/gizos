"""Check the encrypted package action graph without running it."""

load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts")
load("//projects/h2loader/tools/bazel:h2loader_tar_zlib.bzl", "FirmwareReleaseInfo", "h2loader_encrypted_package")
load("//projects/h2loader/tools/bazel:h2loader_zlib_tar.bzl", "h2loader_zlib_tar")

def _action_impl(ctx):
    env = analysistest.begin(ctx)
    target = analysistest.target_under_test(env)
    source = ctx.attr.plain_source[FirmwareReleaseInfo]
    actions = [a for a in analysistest.target_actions(env) if a.mnemonic == "H2LoaderEncryptedPackage"]
    asserts.equals(env, 1, len(actions))
    action = actions[0]

    # A flag transition moves the target to another output root, so compare
    # configuration-independent short paths.
    inputs = [f.short_path for f in action.inputs.to_list()]
    for plain in source.release_files.to_list():
        asserts.false(env, plain.short_path in inputs, "encrypted action consumed " + plain.short_path)
    asserts.true(env, source.native.app_image.short_path in inputs)
    asserts.true(env, source.native.partition_metadata.short_path in inputs)
    asserts.true(env, ctx.file.public_key.short_path in inputs)
    asserts.equals(env, {}, action.env)
    asserts.true(env, "--flash-aes-key" in action.argv)
    asserts.true(env, ctx.file.public_key.path in action.argv)
    release = target[FirmwareReleaseInfo]
    asserts.true(env, release.encrypted)
    asserts.false(env, source.encrypted)
    for field in ("board", "image", "role", "target", "version", "entry", "package_format", "package_data", "package_data_root"):
        asserts.equals(env, getattr(source, field), getattr(release, field))
    asserts.equals(env, source.native.app_image.short_path, release.native.app_image.short_path)
    return analysistest.end(env)

_PUBLIC_KEY = "//projects/h2loader/tools/bazel:tests/fixtures/flash_aes_public_test.key"

# The skylib transition resolves plain strings against its own repository.
_KEY_FLAG = str(Label("//projects/h2loader/tools/bazel:encryption_key"))
_ACTION_ATTRS = {
    "plain_source": attr.label(mandatory = True, providers = [FirmwareReleaseInfo]),
    "public_key": attr.label(default = _PUBLIC_KEY, allow_single_file = True),
}

_action_test = analysistest.make(_action_impl, attrs = _ACTION_ATTRS)
_flag_action_test = analysistest.make(
    _action_impl,
    attrs = _ACTION_ATTRS,
    config_settings = {_KEY_FLAG: str(Label(_PUBLIC_KEY))},
)

def _failure_impl(ctx):
    env = analysistest.begin(ctx)
    asserts.expect_failure(env, ctx.attr.expected_message)
    return analysistest.end(env)

_FAILURE_ATTRS = {"expected_message": attr.string(mandatory = True)}
_failure_test = analysistest.make(_failure_impl, expect_failure = True, attrs = _FAILURE_ATTRS)
_unset_key_test = analysistest.make(
    _failure_impl,
    expect_failure = True,
    attrs = _FAILURE_ATTRS,
    config_settings = {_KEY_FLAG: str(Label("//projects/h2loader/tools/bazel:no_encryption_key"))},
)

def encrypted_package_analysis_tests(name, plain_source, plain_esp_source, encrypted_target, compatibility):
    _action_test(
        name = name + "_action_test",
        plain_source = plain_source,
        target_compatible_with = compatibility,
        target_under_test = encrypted_target,
    )
    h2loader_encrypted_package(name = name + "_unset_key", testonly = True, source = plain_source)
    _flag_action_test(
        name = name + "_flag_key_test",
        plain_source = plain_source,
        target_compatible_with = compatibility,
        target_under_test = ":" + name + "_unset_key",
    )
    _unset_key_test(
        name = name + "_unset_key_test",
        expected_message = "encrypted packages require one key file",
        target_compatible_with = compatibility,
        target_under_test = ":" + name + "_unset_key",
    )
    h2loader_zlib_tar(name = name + "_plain_from_encrypted", testonly = True, source = encrypted_target, tags = ["manual"])
    _failure_test(
        name = name + "_plain_from_encrypted_test",
        expected_message = "package source must be plaintext",
        target_compatible_with = compatibility,
        target_under_test = ":" + name + "_plain_from_encrypted",
    )
    h2loader_encrypted_package(name = name + "_encrypted_from_encrypted", testonly = True, source = encrypted_target, encryption_key = _PUBLIC_KEY)
    _failure_test(
        name = name + "_encrypted_from_encrypted_test",
        expected_message = "package source must be plaintext",
        target_compatible_with = compatibility,
        target_under_test = ":" + name + "_encrypted_from_encrypted",
    )
    h2loader_encrypted_package(name = name + "_unsupported_platform", testonly = True, source = plain_esp_source, encryption_key = _PUBLIC_KEY)
    _failure_test(
        name = name + "_unsupported_platform_test",
        expected_message = "encrypted packages support BK7258 only",
        target_compatible_with = compatibility,
        target_under_test = ":" + name + "_unsupported_platform",
    )
    native.filegroup(name = name + "_multiple_keys", srcs = [_PUBLIC_KEY, "tests/fixtures/encrypted_package_data/config.txt"])
    h2loader_encrypted_package(name = name + "_multiple_key_files", testonly = True, source = plain_source, encryption_key = ":" + name + "_multiple_keys")
    _failure_test(
        name = name + "_multiple_key_files_test",
        expected_message = "encrypted packages require one key file",
        target_compatible_with = compatibility,
        target_under_test = ":" + name + "_multiple_key_files",
    )
