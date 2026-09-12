"""Analysis tests for h2_lua_web_app() argument validation."""

load("@bazel_skylib//lib:unittest.bzl", "asserts", "unittest")
load(":lua_web_app.bzl", "lua_web_app_argument_error")

def _argument_errors_impl(ctx):
    env = unittest.begin(ctx)
    asserts.equals(env, "", lua_web_app_argument_error())
    asserts.equals(env, "", lua_web_app_argument_error(4294967295))
    for run_ms, shown in ((-1, "-1"), (4294967296, "4294967296"), ("1500", '"1500"')):
        asserts.equals(
            env,
            "run_ms %s must be an int in 0..4294967295" % shown,
            lua_web_app_argument_error(run_ms),
        )
    return unittest.end(env)

lua_web_app_argument_test = unittest.make(_argument_errors_impl)
