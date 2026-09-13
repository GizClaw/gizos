"""Run one embedded Lua script as an app_host Web App on a web board."""

load("@bazel_skylib//rules:write_file.bzl", "write_file")
load("@rules_cc//cc:defs.bzl", "cc_library")
load("//libs/app_host:web_app.bzl", "h2_web_app")
load("//libs/app_host:web_board.bzl", "H2WebBoardInfo")
load("//libs/lua:lua_resource.bzl", "h2_lua_resource")

_MAX_RUN_MS = 4294967295  # h2_web_app_host_config_t.run_ms is uint32_t
_DEFAULT_VM_BYTES = 512 * 1024
_DEFAULT_SOURCE_BYTES = 128 * 1024
_ARG_NAME_CHARS = "abcdefghijklmnopqrstuvwxyz0123456789_"

def lua_web_app_argument_error(
        run_ms = 0,
        vm_memory_limit_bytes = _DEFAULT_VM_BYTES,
        source_limit_bytes = _DEFAULT_SOURCE_BYTES,
        script_args = {}):
    """Returns why h2_lua_web_app() arguments are invalid, or "" when valid."""
    if type(run_ms) != "int" or run_ms < 0 or run_ms > _MAX_RUN_MS:
        return "run_ms %r must be an int in 0..%d" % (run_ms, _MAX_RUN_MS)
    for name, value, low, high in (
        ("vm_memory_limit_bytes", vm_memory_limit_bytes, 65536, 16777216),
        ("source_limit_bytes", source_limit_bytes, 1, 1048576),
    ):
        if type(value) != "int" or value < low or value > high:
            return "%s %r must be an int in %d..%d" % (name, value, low, high)
    if type(script_args) != "dict" or len(script_args) > 16:
        return "script_args must be a string dictionary with at most 16 entries"
    for name, value in script_args.items():
        if type(name) != "string" or not name or len(name) > 32 or [c for c in name.elems() if c not in _ARG_NAME_CHARS]:
            return "script_args name %r must use 1..32 ASCII [a-z0-9_] characters" % name
        if type(value) != "string" or len(value) > 256 or [c for c in value.elems() if c < " " or c > "~"]:
            return "script_args[%r] must be at most 256 printable ASCII characters" % name
    return ""

def lua_web_app_board_argument_error(script_arg_names, button_names):
    """Returns the analysis error for app arguments shadowing board inputs."""
    for name in script_arg_names:
        if name in button_names:
            return "script_args name %r conflicts with a board Button" % name
    return ""

def _c_string(value):
    # C11 trigraphs must not reinterpret printable sequences such as ??/.
    return '"%s"' % value.replace("\\", "\\\\").replace('"', '\\"').replace("?", "\\?")

def _exit_button_check_impl(ctx):
    board = ctx.attr.board[H2WebBoardInfo]
    error = lua_web_app_board_argument_error(ctx.attr.script_arg_names, board.button_names)
    if error:
        fail("%s: %s" % (ctx.label, error))
    if ctx.attr.exit_button and ctx.attr.exit_button not in board.button_names:
        fail("%s: exit_button %r is not a Button of %s (Buttons: %s)" % (
            ctx.label,
            ctx.attr.exit_button,
            ctx.attr.board.label,
            ", ".join(board.button_names) or "none",
        ))
    return [DefaultInfo()]

_exit_button_check = rule(
    implementation = _exit_button_check_impl,
    attrs = {
        "board": attr.label(mandatory = True, providers = [H2WebBoardInfo]),
        "exit_button": attr.string(),
        "script_arg_names": attr.string_list(),
    },
)

def h2_lua_web_app(
        name,
        script,
        board,
        skin = None,
        exit_button = None,
        extension = None,
        run_ms = 0,
        vm_memory_limit_bytes = _DEFAULT_VM_BYTES,
        source_limit_bytes = _DEFAULT_SOURCE_BYTES,
        script_args = {},
        **kwargs):
    """Declares `<name>` (web tar), `serve` and `browser_test` for one script.

    The script is embedded with h2_lua_resource() and run by a generic App
    entry on app_host, on the given web board. Every board Button reaches the
    script as `args.<name>` (its Runtime component id, 1..N in board order)
    and its Runtime events are dispatched to the job. The exit Button ends
    the job instead; so does the page's Stop button. Use one h2_lua_web_app
    per package.

    Args:
      name: Archive target name; also the `H2_WEB_APP name=` marker.
      script: Label of the `.lua` source.
      board: h2_web_board() target (display, Buttons, skins).
      skin: Name of one of the board's skins; defaults to its default_skin.
      exit_button: Optional board Button name that cancels the job.
      extension: Optional cc_library defining `h2_web_lua_app_extension`
        (//libs/lua/web:lua_app_extension) to register modules or
        capabilities and to decide when the exit Button ends the job.
      run_ms: Nonzero stops the App after this long exactly as the page Stop
        button does (for tests).
      **kwargs: Passed to h2_web_app() (passes, fails, presses, taps, clicks,
        evals, canvas_min, test_timeout_s, ...); `deps` and `linkopts` are
        appended to the macro's own, and `srcs`/`app_name` are rejected.
      vm_memory_limit_bytes: VM budget in bytes, 65536..16777216; default 524288.
      source_limit_bytes: Source budget in bytes, 1..1048576; default 131072.
      script_args: At most 16 app-owned string arguments. Names use 1..32
        ASCII [a-z0-9_] characters and cannot shadow board Button names;
        values use at most 256 printable ASCII characters (empty allowed).
    """
    error = lua_web_app_argument_error(run_ms, vm_memory_limit_bytes, source_limit_bytes, script_args)
    if error:
        fail("h2_lua_web_app: " + error)
    for fixed in ("srcs", "app_name"):
        if fixed in kwargs:
            fail("h2_lua_web_app: %s is set by the macro" % fixed)
    symbol = name.replace("-", "_") + "_script"

    h2_lua_resource(
        name = name + "_script",
        src = script,
        symbol = symbol,
    )

    _exit_button_check(
        name = name + "_exit_button_check",
        board = board,
        exit_button = exit_button or "",
        script_arg_names = sorted(script_args.keys()),
    )

    write_file(
        name = name + "_config_header",
        out = name + "_config/h2_web_lua_app_config.h",
        content = [
            "#ifndef H2_WEB_LUA_APP_CONFIG_H",
            "#define H2_WEB_LUA_APP_CONFIG_H",
            "/* Generated by h2_lua_web_app(%s). */" % name,
            '#include "%s_script_generated.h"' % name,
            "#define H2_WEB_LUA_APP_NAME %s" % _c_string(name),
            "#define H2_WEB_LUA_APP_SOURCE %s" % symbol,
            "#define H2_WEB_LUA_APP_SOURCE_SIZE %s_size" % symbol,
            "#define H2_WEB_LUA_APP_EXIT_BUTTON %s" % _c_string(exit_button or ""),
            "#define H2_WEB_LUA_APP_EXTENSION %d" % (1 if extension else 0),
            "#define H2_WEB_LUA_APP_RUN_MS %du" % run_ms,
            "#define H2_WEB_LUA_APP_VM_BYTES %du" % vm_memory_limit_bytes,
            "#define H2_WEB_LUA_APP_SOURCE_LIMIT_BYTES %du" % source_limit_bytes,
            "#define H2_WEB_LUA_APP_ARG_COUNT %du" % len(script_args),
            '#include "h2_lua_job.h"',
            "static const h2_lua_arg_t h2_web_lua_script_args[] = {",
        ] + [
            "  {%s, %s}," % (_c_string(key), _c_string(script_args[key]))
            for key in sorted(script_args.keys())
        ] + [
            "  {NULL, NULL}, /* Sentinel keeps the empty configuration valid C. */",
            "};",
            "#endif",
            "",
        ],
    )

    cc_library(
        name = name + "_config",
        hdrs = [name + "_config/h2_web_lua_app_config.h"],
        data = [":" + name + "_exit_button_check"],
        strip_include_prefix = name + "_config",
        deps = [":" + name + "_script", Label("//libs/lua")],
    )

    linkopts = kwargs.pop("linkopts", [])
    deps = kwargs.pop("deps", [])
    h2_web_app(
        name = name,
        srcs = [Label("//libs/lua/web:src/h2_web_lua_app.c")],
        app_name = name,
        board = board,
        skin = skin,
        linkopts = ["-sASYNCIFY_REMOVE=['lua*','yyjson*']"] + linkopts,
        deps = [
            ":" + name + "_config",
            Label("//libs/lua"),
            Label("//libs/lua/web:lua_app_extension"),
        ] + ([extension] if extension else []) + deps,
        **kwargs
    )
