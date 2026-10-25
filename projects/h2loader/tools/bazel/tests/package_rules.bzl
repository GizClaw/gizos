"""Host-only native input fixture for real package-rule action tests."""

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
