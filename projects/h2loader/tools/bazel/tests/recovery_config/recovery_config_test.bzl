"""Analysis tests for the public H2Loader recovery-config contract."""

load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts")
load("//tools/bazel:bk7258.bzl", "Bk7258FirmwareInfo")

def _fake_bk7258_firmware_impl(ctx):
    ap_elf = ctx.actions.declare_file(ctx.label.name + "/ap.elf")
    ap_image = ctx.actions.declare_file(ctx.label.name + "/ap.bin")
    ap_map = ctx.actions.declare_file(ctx.label.name + "/ap.map")
    cp_elf = ctx.actions.declare_file(ctx.label.name + "/cp.elf")
    cp_image = ctx.actions.declare_file(ctx.label.name + "/cp.bin")
    cp_map = ctx.actions.declare_file(ctx.label.name + "/cp.map")
    managed_app_image = ctx.actions.declare_file(ctx.label.name + "/app_ab_crc.rbl")
    recovery_image = ctx.actions.declare_file(ctx.label.name + "/all-app.bin")
    partition_metadata = ctx.actions.declare_file(ctx.label.name + "/partition-metadata.json")
    outputs = [
        ap_elf,
        ap_image,
        ap_map,
        cp_elf,
        cp_image,
        cp_map,
        managed_app_image,
        recovery_image,
        partition_metadata,
    ]
    for output in outputs:
        ctx.actions.write(output, "fixture")
    return [
        DefaultInfo(files = depset(outputs)),
        Bk7258FirmwareInfo(
            ap_elf = ap_elf,
            ap_image = ap_image,
            ap_map = ap_map,
            cp_elf = cp_elf,
            cp_image = cp_image,
            cp_map = cp_map,
            files = depset(outputs),
            managed_app_image = managed_app_image,
            partition_metadata = partition_metadata,
            recovery_image = recovery_image,
            target = "bk7258",
            version = "1.2.3",
        ),
    ]

fake_bk7258_firmware = rule(implementation = _fake_bk7258_firmware_impl)

def _assert_recovery_config(env, suffix, message):
    actions = [
        action
        for action in analysistest.target_actions(env)
        if action.mnemonic == "H2LoaderTarZlib"
    ]
    asserts.equals(env, 1, len(actions))
    if not actions:
        return
    argv = actions[0].argv
    path = ""
    for index in range(len(argv) - 1):
        if argv[index] == "--bk-recovery-config":
            path = argv[index + 1]
            break
    asserts.true(env, path.endswith(suffix), message)
    asserts.true(env, path in [input_file.path for input_file in actions[0].inputs.to_list()], "recovery config must be a declared package action input")

def _explicit_recovery_config_test_impl(ctx):
    env = analysistest.begin(ctx)
    _assert_recovery_config(env, "/private_recovery.json", "explicit recovery config must reach the package action")
    return analysistest.end(env)

explicit_recovery_config_test = analysistest.make(_explicit_recovery_config_test_impl)

def _fallback_recovery_config_test_impl(ctx):
    env = analysistest.begin(ctx)
    _assert_recovery_config(env, "boards/bk7258_v3_202405/bk7258/layouts/h2loader/bk_loader.json", "built-in public-board fallback must be preserved")
    return analysistest.end(env)

fallback_recovery_config_test = analysistest.make(_fallback_recovery_config_test_impl)

def _invalid_role_recovery_config_test_impl(ctx):
    env = analysistest.begin(ctx)
    asserts.expect_failure(env, "recovery_config is only valid for BK7258 H2Loader firmware")
    return analysistest.end(env)

invalid_role_recovery_config_test = analysistest.make(
    _invalid_role_recovery_config_test_impl,
    expect_failure = True,
)

def _invalid_target_recovery_config_test_impl(ctx):
    env = analysistest.begin(ctx)
    asserts.expect_failure(env, "recovery_config is only valid for BK7258 H2Loader firmware")
    return analysistest.end(env)

invalid_target_recovery_config_test = analysistest.make(
    _invalid_target_recovery_config_test_impl,
    expect_failure = True,
)
