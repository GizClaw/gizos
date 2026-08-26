"""Target-owned task-policy components for native firmware."""

load("@rules_cc//cc:defs.bzl", "cc_test")
load("//tools/bazel:cc_options.bzl", "H2_C11_OPTS", "H2_WARNING_COPTS")
load("//tools/bazel:native_component.bzl", "firmware_native_component")

_HOST_COMPATIBILITY = select({
    "//tools/bazel/platforms:host_linux_target_linux": [],
    "//tools/bazel/platforms:host_macos_target_macos": [],
    "//tools/bazel/platforms:host_windows_target_windows": [],
    "//conditions:default": ["@platforms//:incompatible"],
})

def _target_directory(relative_directory):
    package = native.package_name()
    return package + "/" + relative_directory if package else relative_directory

def esp_target_task_policy(name = "task_policy", directory = "task_policy"):
    """Declares one ESP policy owned by the calling firmware target package."""
    header = directory + "/h2_esp_target_task_policy.h"
    source = directory + "/h2_esp_target_task_policy.c"
    test_source = directory + "/tests/test_h2_esp_target_task_policy.c"
    firmware_native_component(
        name = name,
        hdrs = [header],
        srcs = [source],
        component_directory = _target_directory(directory),
        component_name = "h2_esp_target_task_policy",
        data = [directory + "/CMakeLists.txt"],
        deps = ["//native_component_src/esp-idf6.x/h2_pal_core"],
    )
    cc_test(
        name = name + "_test",
        srcs = [
            header,
            source,
            test_source,
        ],
        conlyopts = H2_C11_OPTS,
        copts = H2_WARNING_COPTS,
        includes = [directory],
        target_compatible_with = _HOST_COMPATIBILITY,
        deps = [
            "//libs/pal",
            "//libs/trie",
            "//native_component_src/esp-idf6.x/h2_target_task_policy:test_sdk",
        ],
    )

def bk7258_target_task_policy(
        ap_name = "ap_task_policy",
        cp_name = "cp_task_policy",
        directory = "task_policy"):
    """Declares AP and CP policies owned by one BK7258 firmware target."""
    ap_directory = directory + "/ap"
    cp_directory = directory + "/cp"
    ap_header = ap_directory + "/h2_bk_target_task_policy.h"
    cp_header = cp_directory + "/h2_bk_target_task_policy.h"
    ap_source = ap_directory + "/h2_bk_target_task_policy.c"
    cp_source = cp_directory + "/h2_bk_target_task_policy.c"
    ap_test_source = ap_directory + "/tests/test_h2_bk_target_task_policy.c"
    cp_test_source = cp_directory + "/tests/test_h2_bk_target_task_policy.c"
    firmware_native_component(
        name = ap_name,
        hdrs = [ap_header],
        srcs = [ap_source],
        component_directory = _target_directory(ap_directory),
        component_name = "h2_bk_target_task_policy",
        data = [ap_directory + "/CMakeLists.txt"],
        execution_unit = "ap",
        deps = ["//native_component_src/bk7258/ap/h2_pal_core"],
    )
    firmware_native_component(
        name = cp_name,
        hdrs = [cp_header],
        srcs = [cp_source],
        component_directory = _target_directory(cp_directory),
        component_name = "h2_bk_target_task_policy",
        data = [cp_directory + "/CMakeLists.txt"],
        execution_unit = "cp",
        deps = ["//native_component_src/bk7258/cp/h2_pal_core"],
    )
    cc_test(
        name = ap_name + "_test",
        srcs = [
            ap_header,
            ap_source,
            ap_test_source,
        ],
        conlyopts = H2_C11_OPTS,
        copts = H2_WARNING_COPTS,
        includes = [ap_directory],
        target_compatible_with = _HOST_COMPATIBILITY,
        deps = [
            "//libs/pal",
            "//libs/trie",
            "//native_component_src/bk7258/ap/h2_target_task_policy:test_sdk",
        ],
    )
    cc_test(
        name = cp_name + "_test",
        srcs = [
            cp_header,
            cp_source,
            cp_test_source,
        ],
        conlyopts = H2_C11_OPTS,
        copts = H2_WARNING_COPTS,
        includes = [cp_directory],
        target_compatible_with = _HOST_COMPATIBILITY,
        deps = [
            "//libs/pal",
            "//native_component_src/bk7258/cp/h2_target_task_policy:test_sdk",
        ],
    )
