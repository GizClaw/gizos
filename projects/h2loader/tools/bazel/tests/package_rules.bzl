"""Host-only native input fixture for real package-rule action tests."""

load("//tools/bazel:bk7258.bzl", "Bk7258FirmwareInfo")
load("//tools/bazel:esp_idf.bzl", "FirmwareInfo")
load("//tools/bazel:firmware.bzl", "FirmwareVersionInfo")

def _impl(ctx):
    image = ctx.actions.declare_file(ctx.label.name + "/app.bin")
    ctx.actions.write(image, "synthetic shared raw App; never a hardware image\n")
    return [DefaultInfo(files = depset([image])), FirmwareInfo(
        app_image = image,
        elf = image,
        map = image,
        bootloader_image = image,
        combined_factory_image = image,
        partition_table_image = image,
        flash_files = image,
        flash_metadata = image,
        target = "esp32s3",
        version = ctx.attr.version[FirmwareVersionInfo].value,
    )]

package_rules_firmware = rule(
    implementation = _impl,
    attrs = {"version": attr.label(mandatory = True, providers = [FirmwareVersionInfo])},
)

def _bk_impl(ctx):
    root = ctx.label.name + "/"
    managed = ctx.actions.declare_file(root + "app_ab_crc.rbl")
    recovery = ctx.actions.declare_file(root + "all-app.bin")
    metadata = ctx.actions.declare_directory(root + "partition-metadata")
    config = ctx.actions.declare_file(root + "recovery.json")
    debug = ctx.actions.declare_file(root + "debug.txt")
    ctx.actions.write(debug, "synthetic BK debug output; never a hardware image\n")
    args = ctx.actions.args()
    args.add("--role", ctx.attr.role)
    args.add("--managed", managed)
    args.add("--recovery", recovery)
    args.add("--metadata", metadata.path)
    args.add("--config", config)
    ctx.actions.run(
        arguments = [args],
        executable = ctx.executable._writer,
        mnemonic = "H2LoaderBkPackageFixture",
        outputs = [managed, recovery, metadata, config],
    )
    files = depset([managed, recovery, metadata, config, debug])
    return [DefaultInfo(files = files), Bk7258FirmwareInfo(
        ap_elf = debug,
        ap_image = debug,
        ap_map = debug,
        cp_elf = debug,
        cp_image = debug,
        cp_map = debug,
        files = files,
        managed_app_image = managed,
        partition_metadata = metadata,
        recovery_config = config,
        recovery_image = recovery,
        target = "bk7258",
        version = ctx.attr.version[FirmwareVersionInfo].value,
    )]

bk_package_rules_firmware = rule(
    implementation = _bk_impl,
    attrs = {
        "role": attr.string(mandatory = True, values = ["app", "h2loader"]),
        "version": attr.label(mandatory = True, providers = [FirmwareVersionInfo]),
        "_writer": attr.label(default = "//projects/h2loader/tools/bazel:bk_package_fixture_writer", cfg = "exec", executable = True),
    },
)
