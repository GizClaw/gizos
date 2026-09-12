"""Run a serve-ready Web archive through a local development server."""

load("@rules_python//python:defs.bzl", "py_binary", "py_test")

_SERVER = Label("//tools/bazel:web_archive_server.py")
_HOST_LINUX = Label("//tools/bazel/platforms:host_linux_target_linux")
_HOST_MACOS = Label("//tools/bazel/platforms:host_macos_target_macos")
_HOST_WINDOWS = Label("//tools/bazel/platforms:host_windows_target_windows")

def web_archive_serve(name, archive, visibility = None, tags = None):
    """Creates a `bazel run` target that serves one tar archive.

    Args:
      name: Executable target name.
      archive: Label of the serve-ready tar archive.
      visibility: Optional target visibility.
      tags: Optional Bazel tags.
    """
    py_binary(
        name = name,
        srcs = [_SERVER],
        args = [
            "--archive",
            "$(location %s)" % archive,
        ],
        data = [archive],
        legacy_create_init = 0,
        main = _SERVER,
        tags = tags or [],
        target_compatible_with = select({
            _HOST_LINUX: [],
            _HOST_MACOS: [],
            _HOST_WINDOWS: [],
            "//conditions:default": ["@platforms//:incompatible"],
        }),
        visibility = visibility,
    )

def _shell_quote(value):
    # Test args are shell-tokenized; keep regexes with spaces intact.
    return "'" + value.replace("'", "'\\''") + "'"

_BROWSER_TEST = Label("//tools/bazel:web_archive_browser_test.py")
_WEBRTC_SERVER = Label("//tools/webrtc-test-server:webrtc_test_server")

def web_archive_browser_test(
        name,
        archive,
        passes,
        fails = [],
        query = "",
        timeout_s = 60,
        start = True,
        offline = None,
        webrtc_server = False,
        canvas_min = 0,
        presses = [],
        taps = [],
        size = "medium",
        tags = None,
        visibility = None):
    """Runs one Web archive in pinned headless Chromium and judges its console.

    Args:
      name: Test target name.
      archive: Label of the serve-ready tar archive.
      passes: Regexes that must all appear on the console for a pass.
      fails: Regexes that fail the run; aborts and uncaught errors always do.
      query: Query string appended to the page URL.
      timeout_s: Seconds allowed for every pass pattern to appear.
      start: Click the shell's #start button with a user gesture.
      offline: Optional [down, up] regexes that toggle the page offline/online.
      webrtc_server: Start the repository WebRTC fixture and pass
        `stun=` and `signal=` query parameters to the page.
      canvas_min: Require this many non-black #canvas pixels at the end.
      presses: [regex, key] pairs; the key is pressed once regex appears.
      taps: [regex, x, y] triples; #canvas pixel x,y is tapped once regex appears.
      size: Bazel test size.
      tags: Optional Bazel tags.
      visibility: Optional target visibility.
    """
    args = ["--archive", "$(location %s)" % archive, "--timeout", str(timeout_s)]
    for pattern in passes:
        args += ["--pass", _shell_quote(pattern)]
    for pattern in fails:
        args += ["--fail", _shell_quote(pattern)]
    if query:
        args += ["--query", _shell_quote(query)]
    if not start:
        args.append("--no-start")
    if canvas_min:
        args += ["--canvas-min", str(canvas_min)]
    for pattern, key in presses:
        args += ["--press", _shell_quote(pattern), _shell_quote(key)]
    for pattern, x, y in taps:
        args += ["--tap", _shell_quote(pattern), str(x), str(y)]
    if offline:
        args += ["--offline", _shell_quote(offline[0]), _shell_quote(offline[1])]
    data = [archive, _SERVER]
    if webrtc_server:
        args += ["--pion", "$(rootpath %s)" % _WEBRTC_SERVER]
        data.append(_WEBRTC_SERVER)
    py_test(
        name = name,
        size = size,
        srcs = [_BROWSER_TEST, _SERVER],
        args = args,
        data = data + select({
            # Label() resolves the browser repos from GizOS for downstream callers.
            _HOST_LINUX: [Label("@h2_playwright_chromium_linux_x86_64//:runtime")],
            _HOST_MACOS: [Label("@h2_playwright_chromium_macos_arm64//:runtime")],
            "//conditions:default": [],
        }),
        legacy_create_init = 0,
        main = _BROWSER_TEST,
        tags = tags or [],
        target_compatible_with = select({
            _HOST_LINUX: [],
            _HOST_MACOS: [],
            "//conditions:default": ["@platforms//:incompatible"],
        }),
        visibility = visibility,
    )
