"""Build, package, serve and browser-test one app_host Web App target."""

load("@emsdk//emscripten_toolchain:wasm_rules.bzl", "wasm_cc_binary")
load("@rules_cc//cc:defs.bzl", "cc_binary")
load("@rules_pkg//pkg:tar.bzl", "pkg_tar")
load("//tools/bazel:cc_options.bzl", "H2_C11_OPTS", "H2_WARNING_COPTS")
load("//tools/bazel:web_archive.bzl", "web_archive_browser_test", "web_archive_serve")
load("//tools/bazel/platforms:compatibility.bzl", "WEB_WASM32_ARTIFACT_COMPATIBILITY")

_SHELL = Label("//projects/example/libs/web/app_host:shell.html")
_HOSTS = select({
    Label("//tools/bazel/platforms:host_linux_target_linux"): [],
    Label("//tools/bazel/platforms:host_macos_target_macos"): [],
    Label("//tools/bazel/platforms:host_windows_target_windows"): [],
    "//conditions:default": ["@platforms//:incompatible"],
})

def h2_web_app(
        name,
        srcs,
        deps,
        app_name,
        preload = {},
        passes = None,
        fails = [],
        test_timeout_s = 60,
        test_query = "",
        test_offline = None,
        test_webrtc_server = False,
        canvas_min = 0,
        presses = [],
        taps = [],
        linkopts = []):
    """Declares `<name>` (web tar), `serve` and `browser_test` targets.

    Args:
      name: Archive target name; the tar is `<name>.web.tar`.
      srcs: The target's main.c, which calls h2_web_app_host_run().
      deps: The App library and anything main.c needs.
      app_name: Name the host prints in `H2_WEB_APP name=<app_name>` markers.
      preload: Map of file label to absolute path in the Emscripten filesystem.
      passes: Console regexes required for a pass; defaults to the host PASS.
      fails: Extra console regexes that fail the browser test.
      test_timeout_s: Seconds allowed for the browser test.
      test_query: Query string for the browser test page.
      test_offline: Optional [down, up] regexes toggling the page offline.
      test_webrtc_server: Start the WebRTC fixture for the browser test.
      canvas_min: Non-black canvas pixels the App must leave on screen.
      presses: [regex, key] pairs the browser test presses as keys.
      taps: [regex, x, y] canvas taps the browser test performs.
      linkopts: Extra Emscripten link options.
    """
    preload_opts = []
    for label, path in preload.items():
        preload_opts += ["--preload-file", "$(location %s)@%s" % (label, path)]
    cc_binary(
        name = "_wasm/index",
        srcs = srcs,
        additional_linker_inputs = [_SHELL] + preload.keys(),
        conlyopts = H2_C11_OPTS,
        copts = H2_WARNING_COPTS,
        features = ["-output_format_js"],
        linkopts = [
            "-sALLOW_MEMORY_GROWTH=1",
            "-sASSERTIONS=1",
            "-sASYNCIFY=1",
            "-sINITIAL_MEMORY=67108864",
            "-sSTACK_SIZE=1048576",
            "-sNO_EXIT_RUNTIME=1",
            "--shell-file",
            "$(location %s)" % _SHELL,
            "--oformat=html",
        ] + preload_opts + linkopts,
        target_compatible_with = WEB_WASM32_ARTIFACT_COMPATIBILITY,
        deps = deps + [Label("//projects/example/libs/web/app_host")],
    )
    outputs = ["index.html", "index.js", "index.wasm"]
    if preload:
        outputs.append("index.data")
    wasm_cc_binary(
        name = name + "_wasm",
        cc_target = ":_wasm/index",
        outputs = outputs,
        target_compatible_with = _HOSTS,
    )
    pkg_tar(
        name = name,
        srcs = [":" + name + "_wasm"],
        out = name + ".web.tar",
        mode = "0644",
        portable_mtime = True,
        strip_prefix = ".",
        target_compatible_with = _HOSTS,
    )
    web_archive_serve(
        name = "serve",
        archive = ":" + name,
    )
    web_archive_browser_test(
        name = "browser_test",
        archive = ":" + name,
        passes = passes if passes != None else [
            "H2_WEB_APP name=%s result=PASS rc=0 fs=0 destroy=0" % app_name,
        ],
        fails = [
            "H2_WEB_APP name=%s result=FAIL" % app_name,
        ] + fails,
        query = test_query,
        timeout_s = test_timeout_s,
        offline = test_offline,
        webrtc_server = test_webrtc_server,
        canvas_min = canvas_min,
        presses = presses,
        taps = taps,
    )
