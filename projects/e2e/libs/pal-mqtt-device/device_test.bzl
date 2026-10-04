"""Verify root-owned directed installation observations without UART mutation."""

load("@rules_python//python:defs.bzl", "py_test")
load("//tools/bazel/platforms:compatibility.bzl", "HOST_TOOL_COMPATIBILITY")

def _firmware_impl(settings, attr):
    platforms = {"bk7258": "//tools/bazel/platforms:bk7258", "esp32s3": "//tools/bazel/platforms:esp32s3"}
    if attr.firmware_target not in platforms:
        fail("unsupported MQTT device firmware target")
    defines = [value for value in settings["//command_line_option:define"] if not value.startswith("h2_firmware_target=")]
    defines.append("h2_firmware_target=" + attr.firmware_target)
    return {"//command_line_option:platforms": [platforms[attr.firmware_target]], "//command_line_option:define": defines}

_firmware = transition(
    implementation = _firmware_impl,
    inputs = ["//command_line_option:define"],
    outputs = ["//command_line_option:platforms", "//command_line_option:define"],
)

def _package_impl(ctx):
    files = [file for file in ctx.attr.package[DefaultInfo].files.to_list() if file.basename.endswith(".update.tar.zlib")]
    if len(files) != 1:
        fail("MQTT verifier requires one actual managed package")
    return [DefaultInfo(files = depset(files))]

_package = rule(implementation = _package_impl, attrs = {
    "package": attr.label(mandatory = True, cfg = _firmware),
    "firmware_target": attr.string(mandatory = True),
    "_allowlist_function_transition": attr.label(default = "@bazel_tools//tools/allowlists/function_transition_allowlist"),
})

def mqtt_device_test(name, package, target = "bk7258"):
    artifact = name + "_package"
    _package(name = artifact, package = package, firmware_target = target)
    py_test(
        name = name,
        srcs = ["//projects/e2e/libs/pal-mqtt-device:verify_device.py"],
        main = "//projects/e2e/libs/pal-mqtt-device:verify_device.py",
        args = ["--package", "$(rootpath :" + artifact + ")", "--registry", "$(rootpath //projects/e2e/apps/pal-mqtt/app:include/h2_pal_mqtt_cases.inc)"],
        data = [":" + artifact, "//projects/e2e/apps/pal-mqtt/app:include/h2_pal_mqtt_cases.inc"],
        env_inherit = ["H2_MQTT_DEVICE_EVIDENCE_DIR", "H2_MQTT_DEVICE_PORT", "H2_MQTT_DEVICE_UID"],
        legacy_create_init = False,
        local = True,
        tags = ["manual", "external"],
        target_compatible_with = HOST_TOOL_COMPATIBILITY,
    )
