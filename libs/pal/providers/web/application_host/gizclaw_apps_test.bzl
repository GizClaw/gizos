"""IndexedDB persistence regression for a Web Lua catalog consumer."""

load("@rules_python//python:defs.bzl", "py_test")

_RUNNER = Label("//libs/pal/providers/web/application_host/tests/gizclaw_apps:persistence_test.py")
_HELPER = Label("//tools/bazel:web_archive_browser_test.py")
_SERVER = Label("//tools/bazel:web_archive_server.py")
_LINUX = Label("//tools/bazel/platforms:host_linux_target_linux")
_MACOS = Label("//tools/bazel/platforms:host_macos_target_macos")

def web_gizclaw_apps_browser_test(
        name,
        archive,
        app_package,
        app_id,
        version,
        expected_count,
        preinstalled_version = "",
        main_observation = False,
        tags = None):
    """Check local install/list/uninstall and persistence in one page origin.

    Args:
      name: Test target.
      archive: Actual app_host Web tar.
      app_package: A format-1 Lua app archive (one file).
      app_id: Its manifest identity.
      version: Its manifest version.
      expected_count: Initial visible catalog count.
      preinstalled_version: Initial version for an existing identity, or empty.
      main_observation: Also check H106's public native catalog observation.
      tags: Optional Bazel test tags.
    """
    args = [
        "--archive",
        "$(location %s)" % archive,
        "--package",
        "$(location %s)" % app_package,
        "--helper",
        "$(location %s)" % _HELPER,
        "--app-id",
        app_id,
        "--version",
        version,
        "--expected-count",
        str(expected_count),
    ]
    if preinstalled_version:
        args += ["--preinstalled-version", preinstalled_version]
    if main_observation:
        args.append("--main-observation")
    py_test(
        name = name,
        size = "large",
        srcs = [_RUNNER, _HELPER, _SERVER],
        main = _RUNNER,
        args = args,
        data = [archive, app_package] + select({
            _LINUX: [Label("@h2_playwright_chromium_linux_x86_64//:runtime")],
            _MACOS: [Label("@h2_playwright_chromium_macos_arm64//:runtime")],
            "//conditions:default": [],
        }),
        legacy_create_init = 0,
        tags = tags or [],
        target_compatible_with = select({
            _LINUX: [],
            _MACOS: [],
            "//conditions:default": ["@platforms//:incompatible"],
        }),
    )
