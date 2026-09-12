"""Analysis tests for h2_lua_web_app() argument validation."""

load("@bazel_skylib//lib:unittest.bzl", "asserts", "unittest")
load(":lua_web_app.bzl", "lua_web_app_argument_error")

_BUTTONS = {"left": "ArrowLeft", "ok": "Enter", "back": "Escape"}

def _argument_errors_impl(ctx):
    env = unittest.begin(ctx)
    asserts.equals(env, "", lua_web_app_argument_error(_BUTTONS, "back"))
    asserts.equals(env, "", lua_web_app_argument_error(_BUTTONS, None))
    asserts.equals(
        env,
        "buttons needs 1..8 entries",
        lua_web_app_argument_error({}, None),
    )
    asserts.equals(
        env,
        "buttons needs 1..8 entries",
        lua_web_app_argument_error({"b%d" % i: "" for i in range(9)}, None),
    )
    asserts.equals(
        env,
        'Button name "Back" must use [a-z0-9_]',
        lua_web_app_argument_error({"Back": "Escape"}, None),
    )
    asserts.equals(
        env,
        'Button name "" must use [a-z0-9_]',
        lua_web_app_argument_error({"": "Escape"}, None),
    )
    asserts.equals(
        env,
        'exit_button "home" is not in buttons',
        lua_web_app_argument_error(_BUTTONS, "home"),
    )
    asserts.equals(env, "", lua_web_app_argument_error(_BUTTONS, "back", 4294967295))
    for run_ms, shown in ((-1, "-1"), (4294967296, "4294967296"), ("1500", '"1500"')):
        asserts.equals(
            env,
            "run_ms %s must be an int in 0..4294967295" % shown,
            lua_web_app_argument_error(_BUTTONS, "back", run_ms),
        )
    return unittest.end(env)

lua_web_app_argument_test = unittest.make(_argument_errors_impl)
