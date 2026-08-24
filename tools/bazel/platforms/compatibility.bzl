"""Shared target-platform compatibility declarations."""

HOST_TOOL_COMPATIBILITY = select({
    Label("//tools/bazel/platforms:host_linux_target_linux"): [],
    Label("//tools/bazel/platforms:host_macos_target_macos"): [],
    Label("//tools/bazel/platforms:host_windows_target_windows"): [],
    "//conditions:default": ["@platforms//:incompatible"],
})

LINUX_HOST_TOOL_COMPATIBILITY = select({
    Label("//tools/bazel/platforms:host_linux_target_linux"): [],
    "//conditions:default": ["@platforms//:incompatible"],
})

DARWIN_HOST_TOOL_COMPATIBILITY = select({
    Label("//tools/bazel/platforms:host_macos_target_macos"): [],
    "//conditions:default": ["@platforms//:incompatible"],
})

WINDOWS_HOST_TOOL_COMPATIBILITY = select({
    Label("//tools/bazel/platforms:host_windows_target_windows"): [],
    "//conditions:default": ["@platforms//:incompatible"],
})

LINUX_X86_64_ARTIFACT_COMPATIBILITY = select({
    Label("//tools/bazel/platforms:is_linux_x86_64"): [],
    "//conditions:default": ["@platforms//:incompatible"],
})

LINUX_ARM64_ARTIFACT_COMPATIBILITY = select({
    Label("//tools/bazel/platforms:is_linux_arm64"): [],
    "//conditions:default": ["@platforms//:incompatible"],
})

LINUX_ARMV7_GNUEABIHF_ARTIFACT_COMPATIBILITY = select({
    Label("//tools/bazel/platforms:is_linux_armv7_gnueabihf"): [],
    "//conditions:default": ["@platforms//:incompatible"],
})

MACOS_ARM64_ARTIFACT_COMPATIBILITY = select({
    Label("//tools/bazel/platforms:is_macos_arm64"): [],
    "//conditions:default": ["@platforms//:incompatible"],
})

WINDOWS_X86_64_ARTIFACT_COMPATIBILITY = select({
    Label("//tools/bazel/platforms:is_windows_x86_64"): [],
    "//conditions:default": ["@platforms//:incompatible"],
})

IOS_SIM_ARM64_ARTIFACT_COMPATIBILITY = select({
    Label("//tools/bazel/platforms:is_ios_sim_arm64"): ["@platforms//os:ios"],
    "//conditions:default": ["@platforms//:incompatible"],
})

ANDROID_ARM64_ARTIFACT_COMPATIBILITY = select({
    Label("//tools/bazel/platforms:is_android_arm64"): ["@platforms//os:android"],
    "//conditions:default": ["@platforms//:incompatible"],
})

ANDROID_ARM64_PACKAGE_ARTIFACT_COMPATIBILITY = select({
    Label("//tools/bazel/platforms:is_android_arm64"): [],
    "//conditions:default": ["@platforms//:incompatible"],
})

WEB_WASM32_ARTIFACT_COMPATIBILITY = select({
    "@platforms//cpu:wasm32": [],
    "//conditions:default": ["@platforms//:incompatible"],
})

ESP32S3_ARTIFACT_COMPATIBILITY = select({
    Label("//tools/bazel/platforms:is_esp32s3"): [],
    "//conditions:default": ["@platforms//:incompatible"],
})

ESP32P4_ARTIFACT_COMPATIBILITY = select({
    Label("//tools/bazel/platforms:is_esp32p4"): [],
    "//conditions:default": ["@platforms//:incompatible"],
})

BK7258_ARTIFACT_COMPATIBILITY = select({
    Label("//tools/bazel/platforms:is_bk7258"): [],
    "//conditions:default": ["@platforms//:incompatible"],
})

BK3633_ARTIFACT_COMPATIBILITY = select({
    Label("//tools/bazel/platforms:is_bk3633"): [],
    "//conditions:default": ["@platforms//:incompatible"],
})

AC695N_ARTIFACT_COMPATIBILITY = select({
    Label("//tools/bazel/platforms:is_ac695n"): [],
    "//conditions:default": ["@platforms//:incompatible"],
})

AC791N_ARTIFACT_COMPATIBILITY = select({
    Label("//tools/bazel/platforms:is_ac791n"): [],
    "//conditions:default": ["@platforms//:incompatible"],
})

# Targets that the JieLi pi32v2 clang 4.0.1 cannot compile yet (backend
# crashes or unsupported language features) opt out of the pi32v2 graph with
# this select instead of being silently skipped.
PI32V2_UNSUPPORTED_ARTIFACT_COMPATIBILITY = select({
    Label("//tools/bazel/platforms:is_pi32v2"): ["@platforms//:incompatible"],
    "//conditions:default": [],
})
