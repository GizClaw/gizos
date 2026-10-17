"""Reusable ESP32-S3 E2E firmware entries for consumer-owned boards."""

load("//projects/h2loader/tools/bazel:h2loader_firmware.bzl", "h2loader_esp_idf_firmware")
load("//projects/h2loader/tools/bazel:h2loader_tar_zlib.bzl", "h2loader_tar_zlib")
load("//tools/bazel:firmware_components.bzl", "firmware_lib_component")
load("//tools/bazel:native_component.bzl", "firmware_native_component")
load("//tools/bazel:target_task_policy.bzl", "esp_target_task_policy")

_COMMON_LIBRARIES = [
    Label("//libs/atomic"),
    Label("//libs/bleikcp"),
    Label("//libs/bundle"),
    Label("//libs/command"),
    Label("//libs/iostreamikcp"),
    Label("//libs/pal:unsupported"),
    Label("//libs/pal/providers/corehttp"),
    Label("//libs/pal/providers/h2peer"),
    Label("//libs/pal/providers/h2sctp"),
    Label("//libs/pal/providers/libco"),
    Label("//libs/pixa"),
    Label("//libs/runtime"),
    Label("//libs/wifi_sta"),
]

_COMMON_COMPONENTS = [
    Label("//libs/atomic"),
    Label("//libs/runtime"),
    Label("//native_component_src/esp-idf6.x/h2_pal_core"),
    Label("//native_component_src/esp-idf6.x/zlib"),
    Label("//projects/h2loader/native_component_src/esp-idf6.x/h2_h2loader_ble"),
    Label("//projects/h2loader/native_component_src/esp-idf6.x/h2_h2loader_runtime"),
]

_COMMON_POLICIES = [
    "h2loader/appcmd 8 0 4096 psram",
    "h2loader/blecmd 8 0 4096 psram",
    "$h2loader/return 8 0 4096 psram",
    "$h2loader/blelink 6 0 4096 psram",
    "$h2loader/uartcmd default",
    "$bleikcp/kcp 7 0 4096 psram",
    "$bleikcp/server 5 0 4096 psram",
    "$h2peer/net 7 0 4096 psram",
    "$h2peer/udp 7 0 4096 psram",
    "$runtime/input default",
    "$runtime/nfc default",
]

_SUITES = {
    "atomic": struct(
        image = "atomic-e2e",
        libraries = [
            Label("//projects/e2e/apps/atomic/app:atomic_e2e"),
            Label("//projects/e2e/apps/atomic/app:c11_comparison"),
            Label("//projects/e2e/libs/atomic-device:runner"),
        ],
        policies = [
            "atomic/e2e default",
            "atomic/e2e/runner 4 0 65536 psram",
            "atomic/e2e/core0 4 0 8192 internal",
            "atomic/e2e/core1 4 1 8192 internal",
            "atomic/flag/high 9 0 4096 internal",
            "atomic/flag/low 4 0 4096 internal",
        ],
    ),
    "libco-smoke": struct(
        image = "libco-smoke",
        defaults_suite = "libco-smoke",
        headers = [Label("//projects/e2e/libs/esp-device:result_console.h")],
        extra_sources = [Label("//projects/e2e/targets/h2loader_tar_zlib/libco-smoke/devkit:main/confirmation.h")],
        libraries = [Label("//projects/e2e/apps/libco/app:libco_smoke")],
        policies = [],
    ),
    "iperf-client": struct(
        image = "iperf-client",
        libraries = [
            Label("//libs/iperf"),
            Label("//projects/e2e/apps/iperf-client/app:iperf_client"),
            Label("//projects/e2e/apps/iperf/app:iperf_e2e"),
        ],
        policies = ["iperf-client/run 9 0 16384 psram"],
    ),
    "pal-core": struct(
        image = "pal-core",
        libraries = [Label("//projects/e2e/apps/pal-core/app:pal_core_e2e")],
        policies = [
            "pal-core/e2e/core default",
            "pal-core/e2e/runner 4 0 65536 psram",
            "pal-core/e2e/queue default",
            "pal-core/e2e/condition default",
            "pal-core/e2e/consumer default",
            "pal-core/e2e/producer default",
        ],
    ),
    "pal-crypto": struct(
        image = "pal-crypto",
        libraries = [Label("//projects/e2e/libs/pal-crypto-device:runner")],
        policies = ["pal-crypto/e2e/runner 4 0 65536 psram"],
    ),
    "pal-json": struct(
        image = "pal-json",
        libraries = [Label("//projects/e2e/libs/pal-json-device:runner")],
        policies = ["pal-json/e2e/runner 4 0 65536 psram"],
    ),
    "pal-storage": struct(
        image = "pal-storage",
        libraries = [Label("//projects/e2e/libs/pal-storage-device:runner")],
        policies = ["pal-storage/e2e/runner 4 0 65536 psram"],
    ),
}

def _pal_suite(suite):
    return struct(
        image = suite,
        libraries = [Label("//projects/e2e/libs/" + suite + "-device:runner")] + ([Label("//libs/pal/providers/coremqtt")] if suite == "pal-mqtt" else []),
        policies = [suite + "/e2e/runner 4 0 65536 psram"] + (["pal-ipv6/e2e/http-peer 4 0 8192 psram"] if suite == "pal-ipv6" else []),
        wide_network = suite in ["pal-ipv6", "pal-webrtc"],
        variables = ["H2_E2E_WIFI_FIXTURE"] if suite == "pal-mqtt" else [],
        components = [Label("//native_component_src/esp-idf6.x/h2_esp_audio_decoder")] if suite == "pal-audio-decoder" else [],
        headers = [
            Label("//projects/e2e/targets/h2loader_tar_zlib/pal-mqtt/devkit:main/ledger_console.h"),
            Label("//projects/e2e/libs/esp-device:network_provision.h"),
        ] if suite == "pal-mqtt" else [],
    )

_SUITES.update({suite: _pal_suite(suite) for suite in [
    "pal-audio-decoder",
    "pal-http",
    "pal-ipv6",
    "pal-mqtt",
    "pal-net-tls",
    "pal-webrtc",
    "pal-wifi",
]})

_SUITES.update({
    "pal-audio": struct(
        image = "pal-audio",
        source_board = "amoled",
        libraries = [Label("//projects/e2e/apps/pal-audio/app:pal_audio_e2e")],
        policies = ["pal-audio/e2e/runner 4 0 32768 psram"],
    ),
    "pal-display": struct(
        image = "pal-display",
        source_board = "amoled",
        sources = [Label("//projects/e2e/libs/pal-display-device:esp_main.c")],
        libraries = [Label("//projects/e2e/libs/pal-display-device:runner")],
        policies = ["pal-display/e2e/runner 4 0 65536 psram"],
    ),
    "lua-runtime": struct(
        image = "lua-runtime-e2e",
        source_board = "amoled",
        libraries = [Label("//projects/e2e/apps/lua-runtime/app:lua_runtime_e2e")],
        policies = ["lua-runtime/e2e/runner default", "$lua/worker default", "$lua/display default"],
    ),
    "gizclaw-e2e": struct(
        image = "gizclaw-e2e",
        libraries = [Label("//libs/gizclaw"), Label("//projects/e2e/apps/gizclaw/app:gizclaw_e2e"), Label("//projects/e2e/libs/gizclaw-device:ledger")],
        components = [Label("//native_component_src/esp-idf6.x/h2_gizclaw"), Label("//projects/e2e/apps/gizclaw/app:fixture")],
        extra_sources = [Label("//projects/e2e/targets/h2loader_tar_zlib/gizclaw-e2e/devkit:main/" + file) for file in ["h2_gizclaw_e2e_devkit_config.c", "h2_gizclaw_e2e_devkit_state.c"]],
        headers = [Label("//projects/e2e/targets/h2loader_tar_zlib/gizclaw-e2e/devkit:main/" + file) for file in ["h2_gizclaw_e2e_devkit_config.h", "h2_gizclaw_e2e_devkit_state.h", "h2_gizclaw_e2e_devkit_capture.h"]] + [Label("//projects/e2e/libs/esp-device:result_console.h")],
        data = [Label("//projects/e2e/apps/gizclaw:voice_prompt")],
        policies = [
            "$gizclaw/net 5 0 65536 psram",
            "$gizclaw/device 4 any 131072 psram",
            "$gizclaw/audio/download default",
            "$gizclaw/lua/install default",
            "$gizclaw/data-up default",
            "$gizclaw/data-down default",
            "$gizclaw/audio/uplink 16 1 4096 psram",
            "$gizclaw/audio/downlink 16 1 4096 psram",
            "$lua/worker default",
            "$lua/display default",
            "gizclaw/e2e/runner 4 any 131072 psram",
            "gizclaw/e2e/wifi default",
            "gizclaw/e2e/launcher 4 any 4096 psram",
            "gizclaw/e2e/job 4 any 4096 psram",
        ],
    ),
    "iperf": struct(
        image = "iperf",
        libraries = [Label("//libs/iperf"), Label("//projects/e2e/apps/iperf/app:iperf_e2e")],
        policies = [],
        variables = ["H2_IPERF_SERVER", "H2_IPERF_POWER_SAVE", "H2_IPERF_BLE_ADV", "H2_IPERF_ROLE_SERVER", "H2_E2E_WIFI_FIXTURE"],
        headers = [Label("//projects/e2e/libs/esp-device:network_provision.h")],
    ),
    "webrtc-performance": struct(
        image = "webrtc-performance",
        libraries = [Label("//projects/e2e/apps/webrtc-performance/app:webrtc_performance")],
        policies = [],
        variables = ["H2_WEBRTC_PERF_ENDPOINT", "H2_WEBRTC_PERF_STUN_URL"],
    ),
})

_SUITES.update({"lua-link-" + role: struct(
    image = "lua-link-e2e",
    source_suite = "lua-link",
    source_board = "devkit" if role == "host" else "amoled",
    headers = [Label("//projects/e2e/libs/esp-device:result_console.h")],
    libraries = [Label("//projects/e2e/apps/lua-link/app:lua_link_e2e")],
    policies = ["$lua/worker default", "$lua/display default", "$lua/link default"],
) for role in ["host", "join"]})

_SUITES.update({
    "pal-wifi-fixture": struct(
        image = "pal-wifi-fixture",
        source_suite = "pal-wifi",
        source_board = "amoled-fixture",
        defaults_suite = "pal-wifi",
        libraries = [Label("//projects/e2e/libs/pal-wifi-device:runner")],
        policies = ["pal-wifi/e2e/runner 4 0 65536 psram"],
    ),
    "pal-ipv6-fixture": struct(
        image = "pal-ipv6-fixture",
        source_suite = "pal-ipv6",
        source_board = "amoled-fixture",
        defaults_suite = "",
        libraries = [Label("//projects/e2e/libs/pal-ipv6-board-fixture:fixture")],
        extra_sources = [Label("//projects/e2e/targets/h2loader_tar_zlib/pal-ipv6/amoled-fixture:main/" + file) for file in ["ipv6_ap.c", "tls_server.c"]],
        headers = [Label("//projects/e2e/targets/h2loader_tar_zlib/pal-ipv6/amoled-fixture:main/" + file) for file in ["ipv6_ap.h", "tls_server.h"]],
        policies = [
            "pal-ipv6/fixture/control 4 0 65536 psram",
            "pal-ipv6/fixture/raw 5 0 65536 psram",
            "pal-ipv6/fixture/http 6 0 65536 psram",
            "pal-ipv6/fixture/mqtt 4 0 8192 psram",
            "pal-ipv6/fixture/udp 5 0 8192 psram",
        ],
    ),
})

_PROJECT = Label(":CMakeLists.txt")
_MAIN_CMAKE = Label(":main/CMakeLists.txt")
_MAIN_METADATA = Label(":main/idf_component.yml")

def _esp_e2e_firmware_impl(name, visibility, suite, board, board_component, partition, layout_defaults, use_suite_defaults, libraries, components, policies, launcher_srcs, cmake_definitions):
    spec = _SUITES[suite]
    source_package = "//projects/e2e/targets/h2loader_tar_zlib/" + getattr(spec, "source_suite", suite) + "/" + getattr(spec, "source_board", "devkit")
    source = Label(source_package + ":main/main.c")
    suite_defaults = []
    defaults_suite = getattr(spec, "defaults_suite", suite if suite.startswith("pal-") and suite != "pal-ipv6" else "")
    if use_suite_defaults and defaults_suite:
        suite_defaults = [Label(source_package + ":sdkconfig." + defaults_suite + ".defaults")]
    layout_names = [default.name.split("/")[-1] for default in layout_defaults]
    if "sdkconfig.h2loader.defaults" not in layout_names:
        fail("layout_defaults must contain the board's sdkconfig.h2loader.defaults source file")

    firmware_lib_component(
        name = name + "_libraries",
        deps = _COMMON_LIBRARIES + spec.libraries + libraries,
    )
    firmware_native_component(
        name = name + "_launcher",
        native_srcs = getattr(spec, "sources", [source]) + getattr(spec, "extra_sources", []) + launcher_srcs,
        hdrs = getattr(spec, "headers", []) + ([Label("//projects/e2e/libs/esp-device:result_console.h")] if suite in ["pal-http", "pal-net-tls", "pal-webrtc", "pal-wifi", "pal-mqtt", "iperf", "iperf-client"] else []),
        component_directory = (_MAIN_CMAKE.workspace_root + "/" + _MAIN_CMAKE.package + "/main").lstrip("/"),
        component_name = "main",
        data = [_MAIN_CMAKE, _MAIN_METADATA] + getattr(spec, "data", []),
        tags = ["manual"],
        deps = [
            ":" + name + "_libraries",
            board_component,
        ] + _COMMON_COMPONENTS + getattr(spec, "components", []) + components,
    )
    esp_target_task_policy(
        name = name + "_task_policy",
        directory = name + "_task_policy",
        default_policy = "4 any 4096 internal" if suite == "atomic" else "4 any 4096 psram",
        graph = [":" + name + "_launcher"],
        policies = [policy for policy in _COMMON_POLICIES if not (getattr(spec, "wide_network", False) and any([policy.startswith(prefix) for prefix in ["$h2peer/net ", "$h2peer/udp "]]))] + (["$h2peer/net 7 0 32768 psram", "$h2peer/udp 7 0 8192 psram"] if getattr(spec, "wide_network", False) else []) + spec.policies + policies,
    )
    project_name = "h2_" + suite.replace("-", "_") + "_e2e"
    cmake_definitions = dict(cmake_definitions)
    if suite in ["gizclaw-e2e", "pal-ipv6", "pal-webrtc", "webrtc-performance"]:
        cmake_definitions["H2_ESP_PLATFORM_ENABLE_WEBRTC"] = "ON"
    cmake_definitions.update({
        "H2_E2E_BOARD_DIR": (board_component.workspace_root + "/" + board_component.package).lstrip("/"),
        "H2_E2E_PROJECT_NAME": project_name,
        "H2_E2E_SUITE": suite,
        "H2_E2E_BOARD": board,
        "H2_E2E_CONFIG_DEFAULTS": ";".join(layout_names + [default.name.split("/")[-1] for default in suite_defaults]),
    })
    h2loader_esp_idf_firmware(
        name = name + "_firmware",
        board = board,
        cmake_definitions = cmake_definitions,
        cmake_variables = getattr(spec, "variables", []),
        graph = [":" + name + "_launcher"],
        layout_files = {
            "partition": partition,
            "project_support_files": layout_defaults + suite_defaults,
        },
        project = _PROJECT,
        project_name = project_name,
        task_policy = ":" + name + "_task_policy",
        target = "esp32s3",
    )
    h2loader_tar_zlib(
        name = name,
        board = board,
        firmware = ":" + name + "_firmware",
        image = spec.image,
        role = "app",
        target = "esp32s3",
        visibility = visibility,
    )

esp_e2e_firmware = macro(
    doc = "Builds an existing GizOS E2E launcher with a consumer-owned ESP32-S3 board. The caller owns the board, layout and extra dependencies/task policies; GizOS retains the suite, launcher and SDK build implementation. Only the managed package target is exported.",
    implementation = _esp_e2e_firmware_impl,
    attrs = {
        "suite": attr.string(mandatory = True, values = sorted(_SUITES), configurable = False),
        "board": attr.string(mandatory = True, configurable = False),
        "board_component": attr.label(mandatory = True, configurable = False),
        "partition": attr.label(mandatory = True, allow_single_file = True, configurable = False),
        "layout_defaults": attr.label_list(doc = "Ordered board-owned SDK defaults source files; their basenames are copied into the native project.", mandatory = True, allow_files = True, configurable = False),
        "use_suite_defaults": attr.bool(doc = "Append the suite's diagnostic SDK defaults. Disable to test the caller's product configuration unchanged.", default = True, configurable = False),
        "libraries": attr.label_list(configurable = False),
        "components": attr.label_list(configurable = False),
        "policies": attr.string_list(configurable = False),
        "launcher_srcs": attr.label_list(allow_files = True, configurable = False),
        "cmake_definitions": attr.string_dict(configurable = False),
    },
)
