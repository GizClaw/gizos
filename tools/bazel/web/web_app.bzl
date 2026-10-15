"""Build, package, serve and browser-test one entry-owned Web App target."""

load("@emsdk//emscripten_toolchain:wasm_rules.bzl", "wasm_cc_binary")
load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")
load("@rules_pkg//pkg:tar.bzl", "pkg_tar")
load("//tools/bazel:cc_options.bzl", "H2_C11_OPTS", "H2_WARNING_COPTS")
load("//tools/bazel:web_archive.bzl", "web_archive_browser_test", "web_archive_serve")
load("//tools/bazel/platforms:compatibility.bzl", "WEB_WASM32_ARTIFACT_COMPATIBILITY")
load(":web_board.bzl", "H2WebBoardInfo")

_DEFAULT_BOARD = Label("//tools/bazel/web:default_board")
_HOSTS = select({
    Label("//tools/bazel/platforms:host_linux_target_linux"): [],
    Label("//tools/bazel/platforms:host_macos_target_macos"): [],
    Label("//tools/bazel/platforms:host_windows_target_windows"): [],
    "//conditions:default": ["@platforms//:incompatible"],
})

def _c_string(value):
    return '"%s"' % value.replace("\\", "\\\\").replace('"', '\\"')

def _web_board_page_impl(ctx):
    board = ctx.attr.board[H2WebBoardInfo]
    skin_name = ctx.attr.skin or board.default_skin
    if skin_name and skin_name not in board.skins:
        fail("%s: board %s has no skin %r (skins: %s)" % (
            ctx.label,
            ctx.attr.board.label,
            skin_name,
            ", ".join(sorted(board.skins.keys())) or "none",
        ))
    skin = board.skins[skin_name] if skin_name else ctx.file._default_layout
    args = ctx.actions.args()
    args.add("--shell", ctx.file._shell)
    args.add("--layout", skin)
    args.add("--out", ctx.outputs.shell)
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [args],
        inputs = [ctx.file._shell, skin],
        outputs = [ctx.outputs.shell],
        mnemonic = "H2WebAppSkin",
        progress_message = "Injecting web board skin %{label}",
    )
    buttons = [
        "    {%s, %s}," % (_c_string(name), _c_string(key))
        for name, key in zip(board.button_names, board.button_keys)
    ]
    ctx.actions.write(ctx.outputs.header, "\n".join([
        "/* Generated board metadata: no providers or lifecycle code. */",
        "#ifndef H2_WEB_BOARD_CONFIG_H",
        "#define H2_WEB_BOARD_CONFIG_H",
        "#define H2_WEB_BOARD_WIDTH %d" % board.display_width,
        "#define H2_WEB_BOARD_HEIGHT %d" % board.display_height,
        "#define H2_WEB_BOARD_BUTTON_COUNT %du" % len(buttons),
        "#define H2_WEB_BOARD_MAX_BUTTONS 8u",
        "static const struct { const char *name, *key; } h2_web_board_buttons[] = {",
    ] + (buttons if buttons else ['    {"", ""},']) + [
        "};",
        "#endif",
        "",
    ]))
    return [DefaultInfo(files = depset([ctx.outputs.shell, ctx.outputs.header]))]

_web_board_page = rule(
    implementation = _web_board_page_impl,
    attrs = {
        "board": attr.label(mandatory = True, providers = [H2WebBoardInfo]),
        "shell": attr.output(mandatory = True),
        "skin": attr.string(),
        "header": attr.output(mandatory = True),
        "_default_layout": attr.label(
            default = Label("//tools/bazel/web:default_layout.html"),
            allow_single_file = True,
        ),
        "_shell": attr.label(
            default = Label("//tools/bazel/web:shell.html"),
            allow_single_file = True,
        ),
        "_tool": attr.label(
            default = Label("//tools/bazel/web:inject_layout"),
            executable = True,
            cfg = "exec",
        ),
    },
)

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
        clicks = [],
        evals = [],
        board = None,
        skin = None,
        copts = [],
        linkopts = []):
    """Declares `<name>` (web tar), `serve` and `browser_test` targets.

    Args:
      name: Archive target name; the tar is `<name>.web.tar`.
      srcs: The target's main.c, which owns provider/Runtime/App lifetimes.
      deps: The App library and anything main.c needs.
      app_name: Name the entry prints in `H2_WEB_APP name=<app_name>` markers.
      preload: Map of file label to absolute path in the Emscripten filesystem.
      passes: Console regexes required for a pass; defaults to the entry PASS.
      fails: Extra console regexes that fail the browser test.
      test_timeout_s: Seconds allowed for the browser test.
      test_query: Query string for the browser test page.
      test_offline: Optional [down, up] regexes toggling the page offline.
      test_webrtc_server: Start the WebRTC fixture for the browser test.
      canvas_min: Non-black canvas pixels the App must leave on screen.
      presses: [regex, key] pairs the browser test presses as keys.
      taps: [regex, x, y] canvas taps the browser test performs.
      clicks: [regex, css_selector] page-element clicks the browser test
        performs (pointer press and release at the element centre).
      evals: [regex, javascript] page scripts the browser test evaluates.
      board: h2_web_board() target that supplies the display size, the
        Buttons (name and key) and the skins; defaults to
        //tools/bazel/web:default_board (240x240, no Buttons, plain layout).
        The generated h2_web_board_config.h contains dimensions and ordered
        Button names/keys; main.c decides how to expose them to its Runtime.
      skin: Name of one of the board's skins (an unknown name fails
        analysis). Omitted: the board's default_skin, or the plain
        default_layout.html when the board has none.
      copts: Extra C compile options for srcs.
      linkopts: Extra Emscripten link options.
    """
    _web_board_page(
        name = name + "_board_page",
        board = board if board else _DEFAULT_BOARD,
        shell = name + "_shell.html",
        skin = skin or "",
        header = name + "_board/h2_web_board_config.h",
    )
    shell = ":" + name + "_shell.html"
    preload_opts = []
    for label, path in preload.items():
        preload_opts += ["--preload-file", "$(location %s)@%s" % (label, path)]

    # The board artifact contains only immutable metadata for the entry.
    cc_library(
        name = name + "_board",
        hdrs = [":" + name + "_board/h2_web_board_config.h"],
        strip_include_prefix = name + "_board",
        target_compatible_with = WEB_WASM32_ARTIFACT_COMPATIBILITY,
    )
    cc_binary(
        name = "_wasm/index",
        srcs = srcs,
        additional_linker_inputs = [shell] + preload.keys(),
        conlyopts = H2_C11_OPTS,
        copts = H2_WARNING_COPTS + copts,
        features = ["-output_format_js"],
        linkopts = [
            "-sALLOW_MEMORY_GROWTH=1",
            "-sASSERTIONS=1",
            "-sPROXY_TO_PTHREAD=1",
            "-sPTHREAD_POOL_SIZE=16",
            "-sINITIAL_MEMORY=67108864",
            "-sSTACK_SIZE=1048576",
            "-sNO_EXIT_RUNTIME=1",
            "--shell-file",
            "$(location %s)" % shell,
            "--oformat=html",
        ] + preload_opts + linkopts,
        target_compatible_with = WEB_WASM32_ARTIFACT_COMPATIBILITY,
        deps = deps + [
            ":" + name + "_board",
            Label("//libs/pal/providers/web/pal_core"),
            Label("//libs/atomic/providers/c11"),
        ],
    )
    outputs = ["index.html", "index.js", "index.wasm"]
    if preload:
        outputs.append("index.data")
    wasm_cc_binary(
        name = name + "_wasm",
        threads = "emscripten",
        cc_target = ":_wasm/index",
        outputs = outputs,
        target_compatible_with = _HOSTS,
    )
    native.genrule(
        name = name + "_isolation_headers",
        outs = ["_headers"],
        cmd = "printf '/*\\n  Cross-Origin-Opener-Policy: same-origin\\n  Cross-Origin-Embedder-Policy: require-corp\\n' > $@",
    )
    pkg_tar(
        name = name,
        srcs = [":" + name + "_wasm", ":" + name + "_isolation_headers"],
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
        clicks = clicks,
        evals = evals,
    )
