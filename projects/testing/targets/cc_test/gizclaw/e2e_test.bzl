load("@rules_cc//cc:defs.bzl", "cc_test")
load("//tools/bazel/toolchains:cc_options.bzl", "H2_CXX17_OPTS", "H2_WARNING_COPTS")

def gizclaw_e2e_desktop_deps(backend = "h2peer", app = "//projects/testing/integration/gizclaw/app:gizclaw_e2e"):
    deps = [
        "//projects/testing/integration/gizclaw:app_config_fixture",
        "//libs/sync/atomic",
        "//libs/sync/atomic/providers/c11",
        "//libs/testing:testing_pal",
        "//pal/providers/platforms/desktop/pal_core",
        "//pal/providers/backends/corehttp",
        "//pal",
        "//libs/runtime",
        "//projects/testing/integration/gizclaw/libs/device:ledger",
        "//pal/providers/platforms/desktop/app_support:app_support",
        "//pal/providers/platforms/desktop/app_support:bundle_rpath",
        "//projects/testing/targets/cc_test/gizclaw:desktop_options",
        "//projects/testing/targets/cc_test/gizclaw:desktop_static_guard",
        app,
    ]
    if backend == "pion":
        deps.append("//pal/providers/backends/pion:pion")
    return deps

def gizclaw_e2e_desktop_live_test(name, suite, backend = "h2peer", app = "//projects/testing/integration/gizclaw/app:gizclaw_e2e"):
    if backend not in ["h2peer", "pion"]:
        fail("unsupported GizClaw E2E WebRTC backend: %s" % backend)
    cc_test(
        name = name,
        args = [
            "$(rootpath //projects/testing/integration/gizclaw:voice_prompt)",
            suite,
        ],
        srcs = [
            "h2_gizclaw_e2e_desktop.cpp",
            "h2_gizclaw_e2e_desktop.h",
        ],
        copts = H2_WARNING_COPTS,
        cxxopts = H2_CXX17_OPTS,
        data = [
            "//projects/testing/integration/gizclaw:voice_prompt",
        ],
        env_inherit = [
            "H2_GIZCLAW_E2E_REGISTRATION_TOKEN",
            "H2_GIZCLAW_E2E_SUITE",
            "H2_GIZCLAW_E2E_DEVICE_API_URL",
            "H2_GIZCLAW_E2E_AUDIO_URL",
            "H2_GIZCLAW_E2E_LUA_RECORD_GOLDEN",
            "H2_GIZCLAW_E2E_LUA_PERSISTENCE_FAULT",
        ],
        local_defines = ["H2_GIZCLAW_E2E_USE_PION=1"] if backend == "pion" else [],
        size = "enormous",
        tags = ["manual"],
        target_compatible_with = select({
            "//tools/bazel/platforms:host_linux_target_linux": [],
            "//tools/bazel/platforms:host_macos_target_macos": [],
            "//tools/bazel/platforms:host_windows_target_windows": [],
            "//conditions:default": ["@platforms//:incompatible"],
        }),
        deps = gizclaw_e2e_desktop_deps(backend, app),
    )
