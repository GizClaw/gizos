"""H2Loader-managed native firmware declarations."""

load("//tools/bazel:bk7258.bzl", "bk7258_firmware")
load("//tools/bazel:esp_idf.bzl", "esp_idf_firmware")

_ESP_LAYOUTS = {
    ("esp32s3", "amoled"): struct(root = "//boards/amoled/esp32s3/layouts/h2loader"),
    ("esp32s3", "devkit"): struct(
        root = "//boards/devkit/esp32s3/layouts/h2loader",
        support = ["//boards/devkit/esp32s3/layouts/h2loader:loader_uart.defaults"],
    ),
    ("esp32s3", "szp"): struct(root = "//boards/szp/esp32s3/layouts/h2loader"),
    ("esp32s3", "waveshare_esp32s3_a7670e_4g"): struct(root = "//boards/waveshare_esp32s3_a7670e_4g/esp32s3/layouts/h2loader"),
    ("esp32p4", "waveshare_esp32p4_wifi6_touch_lcd_4_3"): struct(root = "//boards/waveshare_esp32p4_wifi6_touch_lcd_4_3/esp32p4/layouts/h2loader"),
}

_BK7258_BOARDS = {
    ("bk7258", "bk7258_v3_202405"): struct(
        root = "//boards/bk7258_v3_202405/bk7258",
        layouts = ["h2loader", "loader", "media", "wifi_csi"],
    ),
}

def _require_layout(layouts, target, board):
    key = (target, board)
    if key not in layouts:
        fail("unsupported H2Loader firmware layout: %s/%s" % (target, board))
    return layouts[key]

def _support_files(kwargs, layout_files):
    support_files = kwargs.pop("support_files", [])
    return support_files + layout_files

def _require_layout_files(layout_files, required_fields, target, board):
    missing = [field for field in required_fields if field not in layout_files]
    if missing:
        fail("incomplete H2Loader layout files for %s/%s: missing %s" % (
            target,
            board,
            ", ".join(missing),
        ))
    return layout_files

def _require_target_policy(policy, field, target, board):
    if policy == None:
        fail("H2Loader firmware target %s/%s is missing %s" % (target, board, field))
    return policy

def h2loader_esp_idf_firmware(name, board, target, task_policy = None, layout = "h2loader", layout_files = None, **kwargs):
    """Declares ESP-IDF firmware from one repository-owned board layout.

    Every board owns exactly one canonical sdkconfig.defaults; a layout only
    adds hardware/SDK settings such as the partition table, rollback, or a
    registered console/memory variant. The concrete firmware target passes its
    task policy explicitly. There is no named config-profile registry. A
    downstream repository with a private board passes layout_files containing
    its exact partition and project-support labels; the private board is not
    added to GizOS's built-in registry.
    """
    for removed in ("config_profile", "config_profiles"):
        if removed in kwargs:
            fail("h2loader_esp_idf_firmware no longer accepts %s; declare a board layout" % removed)
    if layout_files == None:
        entry = _require_layout(_ESP_LAYOUTS, target, board)
        if layout == "h2loader":
            layout_root = entry.root
        else:
            if layout not in getattr(entry, "variants", []):
                fail("unsupported H2Loader ESP layout: %s/%s" % (board, layout))
            layout_root = entry.root.rsplit("/", 1)[0] + "/" + layout
        layout_files = {
            "partition": entry.root + ":partition.csv",
            "project_support_files": [layout_root + ":sdkconfig.h2loader.defaults"],
            "support_files": getattr(entry, "support", []),
        }
    else:
        layout_files = _require_layout_files(
            layout_files,
            ["partition", "project_support_files"],
            target,
            board,
        )
    if "partition" in kwargs:
        fail("h2loader_esp_idf_firmware owns partition")
    if "project_support_files" in kwargs:
        fail("h2loader_esp_idf_firmware owns project_support_files")
    esp_idf_firmware(
        name = name,
        board = board,
        task_policy = _require_target_policy(task_policy, "task_policy", target, board),
        partition = layout_files["partition"],
        project_support_files = layout_files["project_support_files"],
        support_files = _support_files(kwargs, layout_files.get("support_files", [])),
        target = target,
        **kwargs
    )

def h2loader_bk7258_firmware(
        name,
        board,
        target,
        ap_task_policy = None,
        cp_task_policy = None,
        layout = "h2loader",
        layout_files = None,
        **kwargs):
    """Declares BK7258 firmware from one repository-owned board layout.

    Every layout under boards/<board>/bk7258/layouts/<layout>/ owns one
    complete AP configuration, CP configuration, GPIO selection, RAM-region
    plan, and partition metadata. The concrete firmware target passes its AP
    and CP task policies explicitly. There is no named config-profile registry.
    A downstream repository with a private board passes layout_files containing
    the exact AP/CP config, GPIO, RAM-region, and project-support labels.
    """
    for removed in ("config_profile", "config_profiles", "gpio_profile", "memory_profile"):
        if removed in kwargs:
            fail("h2loader_bk7258_firmware no longer accepts %s; declare a board layout" % removed)
    if layout_files == None:
        entry = _require_layout(_BK7258_BOARDS, target, board)
        if layout not in entry.layouts:
            fail("unsupported H2Loader BK7258 layout: %s/%s" % (board, layout))
        layout_root = entry.root + "/layouts/" + layout
        layout_files = {
            "ap_config": [
                entry.root + ":ap.defaults",
                layout_root + ":ap.defaults",
            ],
            "ap_gpio": layout_root + ":ap_gpio",
            "cp_config": [
                entry.root + ":cp.defaults",
                layout_root + ":cp.defaults",
            ],
            "cp_gpio": layout_root + ":cp_gpio",
            "project_support_files": [layout_root + ":layout"],
            "ram_regions": layout_root + ":ram_regions",
        }
    else:
        layout_files = _require_layout_files(
            layout_files,
            [
                "ap_config",
                "ap_gpio",
                "cp_config",
                "cp_gpio",
                "project_support_files",
                "ram_regions",
            ],
            target,
            board,
        )
    if "project_support_files" in kwargs:
        fail("h2loader_bk7258_firmware owns project_support_files")
    bk7258_firmware(
        name = name,
        ap_task_policy = _require_target_policy(ap_task_policy, "ap_task_policy", target, board),
        board = board,
        cp_task_policy = _require_target_policy(cp_task_policy, "cp_task_policy", target, board),
        ap_config = layout_files["ap_config"],
        cp_config = layout_files["cp_config"],
        ap_gpio = layout_files["ap_gpio"],
        cp_gpio = layout_files["cp_gpio"],
        project_support_files = layout_files["project_support_files"],
        ram_regions = layout_files["ram_regions"],
        support_files = _support_files(kwargs, layout_files.get("support_files", [])),
        target = target,
        **kwargs
    )
