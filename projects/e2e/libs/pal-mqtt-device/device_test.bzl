"""Read directed qualification evidence for the explicitly installed package.

The external host verifier must not rebuild firmware and silently substitute a
new image for the artifact that actually ran on the board. Build :package first;
then provide that immutable file through H2_MQTT_DEVICE_PACKAGE.
"""

load("@rules_python//python:defs.bzl", "py_test")
load("//tools/bazel/platforms:compatibility.bzl", "HOST_TOOL_COMPATIBILITY")

def mqtt_device_test(name, package, target = "bk7258"):
    if target not in ["bk7258", "esp32s3", "esp32s31"]:
        fail("unsupported MQTT device firmware target")
    py_test(
        name = name,
        srcs = ["//projects/e2e/libs/pal-mqtt-device:verify_device.py"],
        main = "//projects/e2e/libs/pal-mqtt-device:verify_device.py",
        args = [
            "--registry", "$(rootpath //projects/e2e/apps/pal-mqtt/app:include/h2_pal_mqtt_cases.inc)",
            "--expected-target", target,
            "--build-label", package,
        ],
        data = ["//projects/e2e/apps/pal-mqtt/app:include/h2_pal_mqtt_cases.inc"],
        env_inherit = ["H2_MQTT_DEVICE_EVIDENCE_DIR", "H2_MQTT_DEVICE_PORT", "H2_MQTT_DEVICE_UID", "H2_MQTT_DEVICE_PACKAGE"],
        legacy_create_init = False,
        local = True,
        tags = ["manual", "external"],
        target_compatible_with = HOST_TOOL_COMPATIBILITY,
    )
