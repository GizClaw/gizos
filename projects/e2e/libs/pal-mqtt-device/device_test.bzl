"""Verify root-owned directed installation observations without UART mutation."""

load("@rules_python//python:defs.bzl", "py_test")

def _package_impl(ctx):
    files = [file for file in ctx.attr.package[DefaultInfo].files.to_list() if file.basename.endswith(".update.tar.zlib")]
    if len(files) != 1:
        fail("MQTT verifier requires one actual managed package")
    return [DefaultInfo(files = depset(files))]

_package = rule(implementation = _package_impl, attrs = {"package": attr.label(mandatory = True)})

def mqtt_device_test(name, package):
    artifact = name + "_package"
    _package(name = artifact, package = package)
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
    )
