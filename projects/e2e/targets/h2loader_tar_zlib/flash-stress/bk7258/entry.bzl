"""BK native raw-Flash stress; private consumers supply their board/layout."""

load("@rules_python//python:defs.bzl", "py_test")
load("//projects/h2loader/tools/bazel:h2loader_firmware.bzl", "h2loader_bk7258_firmware")
load("//projects/h2loader/tools/bazel:h2loader_tar_zlib.bzl", "h2loader_tar_zlib")
load("//tools/bazel:app_version.bzl", "app_version")
load("//tools/bazel:firmware_components.bzl", "firmware_lib_component")
load("//tools/bazel:native_component.bzl", "firmware_native_component")
load("//tools/bazel:target_task_policy.bzl", "bk7258_target_task_policy")

_ROOT = "projects/e2e/targets/h2loader_tar_zlib/flash-stress/bk7258"
_SHARED = Label("//" + _ROOT + ":CMakeLists.txt")

def bk7258_flash_stress(board, board_component, layout_files, baseline_partitions, diagnostic_partitions, board_policies = [], project = _SHARED):
    """Declare a native diagnostic package; no firmware writes happen at build."""
    app_version(name = "version", value = "0.1.0-flash-stress")
    py_test(
        name = "layout_test",
        srcs = [Label("//" + _ROOT + ":tests/test_layout.py")],
        main = Label("//" + _ROOT + ":tests/test_layout.py"),
        args = [
            "--baseline",
            "$(location %s)" % baseline_partitions,
            "--diagnostic",
            "$(location %s)" % diagnostic_partitions,
        ],
        data = [baseline_partitions, diagnostic_partitions],
    )
    firmware_lib_component(
        name = "firmware_lib",
        deps = [
            Label("//libs/audio_mixer"),
            Label("//libs/bleikcp"),
            Label("//libs/bundle"),
            Label("//libs/command"),
            Label("//libs/iostreamikcp"),
            Label("//libs/lvgl"),
            Label("//libs/pal:unsupported"),
            Label("//libs/pal/providers/corehttp"),
            Label("//libs/pal/providers/coremqtt"),
            Label("//libs/pal/providers/h2peer"),
            Label("//libs/pal/providers/h2sctp"),
            Label("//libs/pal/providers/modem/quectel:quectel_modem"),
            Label("//libs/drivers/nfc/fm175xx"),
            Label("//libs/runtime"),
            Label("//libs/utils"),
            Label("//libs/wifi_sta"),
        ],
    )
    firmware_native_component(
        name = "ap",
        deps = [
            Label("//" + _ROOT + ":ap_component"),
            ":firmware_lib",
            board_component,
            Label("//native_component_src/bk7258/ap/h2_pal_core"),
            Label("//native_component_src/bk7258/ap/zlib"),
            Label("//native_component_src/bk7258/cp/h2_pal_core"),
            Label("//projects/h2loader/apps/loader/app:tasks"),
            Label("//projects/h2loader/libs/h2loader"),
            Label("//projects/h2loader/native_component_src/bk7258/ap/h2loader_bk"),
            Label("//projects/h2loader/native_component_src/bk7258/cp/h2loader_cp_launcher"),
        ],
    )
    bk7258_target_task_policy(
        ap_allocator = "sram",
        ap_default_policy = "7 0 8192 psram",
        ap_policies = [
            "h2loader/appcmd 5 0 32768 psram",
            "$h2loader/uartcmd 5 0 8192 psram",
            "$h2loader/return default",
            "$h2loader/blelink default",
            "$bleikcp/kcp default",
            "$bleikcp/server default",
            "$h2peer/net default",
            "$h2peer/udp default",
            "$modem/urc default",
            "$runtime/input default",
            "$runtime/nfc default",
            "$lvgl/g2ddraw default",
            "$lvgl/nemagfx default",
            "$lvgl/pxpdraw default",
            "$lvgl/swdraw default",
        ] + board_policies,
        cp_default_policy = "6 4096 default",
        graph = [":ap"],
    )
    h2loader_bk7258_firmware(
        name = "firmware",
        srcs = [project, Label("//" + _ROOT + ":project.cmake")],
        ap_task_policy = ":ap_task_policy",
        board = board,
        cp_task_policy = ":cp_task_policy",
        graph = [":ap"],
        layout_files = layout_files,
        project = project,
        project_name = "h2_bk_flash_stress",
        target = "bk7258",
        version = ":version",
    )
    h2loader_tar_zlib(
        name = "package",
        board = board,
        firmware = ":firmware",
        image = "flash-stress",
        role = "app",
        target = "bk7258",
    )
