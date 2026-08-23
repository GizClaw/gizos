load("@rules_cc//cc:defs.bzl", "cc_test")
load("//tools/bazel:cc_options.bzl", "H2_CXX17_OPTS", "H2_WARNING_COPTS")

def gizclaw_e2e_desktop_deps(backend = "h2peer"):
    deps = [
        "//libs/pal/providers/desktop/pal_core",
        "//libs/pal/providers/corehttp",
        "//libs/pal",
        "//libs/runtime",
        "//libs/pal/providers/desktop/app_support:app_support",
        "//libs/pal/providers/desktop/app_support:bundle_rpath",
        "//projects/e2e/apps/gizclaw/app:gizclaw_e2e",
    ]
    if backend == "pion":
        deps.append("//libs/pal/providers/pion:pion")
    return deps

def gizclaw_e2e_desktop_live_test(name, suite, backend = "h2peer"):
    if backend not in ["h2peer", "pion"]:
        fail("unsupported GizClaw E2E WebRTC backend: %s" % backend)
    cc_test(
        name = name,
        args = [
            "$(rootpath //projects/e2e/apps/gizclaw:voice_prompt)",
            suite,
            "ap",
        ],
        srcs = [
            "h2_gizclaw_e2e_desktop.cpp",
            "h2_gizclaw_e2e_desktop.h",
            "h2_gizclaw_pal_e2e_access_point.c",
            "h2_gizclaw_pal_e2e_access_point.h",
        ],
        copts = H2_WARNING_COPTS,
        cxxopts = H2_CXX17_OPTS,
        data = [
            "//projects/e2e/apps/gizclaw:voice_prompt",
        ],
        env_inherit = [
            "H2_GIZCLAW_E2E_ENTRY",
            "H2_GIZCLAW_E2E_REGISTRATION_TOKEN",
            "H2_GIZCLAW_E2E_SUITE",
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
        deps = gizclaw_e2e_desktop_deps(backend),
    )
