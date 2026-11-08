"""Analysis tests for h2_lua_web_app() argument validation."""

load("@bazel_skylib//lib:unittest.bzl", "asserts", "unittest")
load(":lua_web_app.bzl", "lua_web_app_argument_error", "lua_web_app_board_argument_error")

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
    for vm in (65536, 524288, 4194304, 16777216):
        asserts.equals(env, "", lua_web_app_argument_error(vm_memory_limit_bytes = vm))
    for source in (1, 131072, 262144, 1048576):
        asserts.equals(env, "", lua_web_app_argument_error(source_limit_bytes = source))
    for vm in (True, "4194304", 0, -1, 65535, 16777217):
        asserts.true(env, "vm_memory_limit_bytes" in lua_web_app_argument_error(vm_memory_limit_bytes = vm))
    for source in (False, "262144", 0, -1, 1048577):
        asserts.true(env, "source_limit_bytes" in lua_web_app_argument_error(source_limit_bytes = source))
    for args in ({}, {"empty": "", "quoted": 'a"b\\c'}, {"a" * 32: "v" * 256}, {"a%d" % i: "v" for i in range(16)}):
        asserts.equals(env, "", lua_web_app_argument_error(script_args = args))
    for args in ([], None, {"a%d" % i: "v" for i in range(17)}, {"": "v"}, {"a" * 33: "v"}, {"UP": "v"}, {"a-b": "v"}, {1: "v"}, {"x": 1}, {"x": "a" * 257}, {"x": "a\nb"}, {"x": "\r"}, {"x": "\t"}, {"x": "中文"}):
        asserts.true(env, "script_args" in lua_web_app_argument_error(script_args = args))
    asserts.equals(env, "", lua_web_app_board_argument_error(["profile"], ["left", "back"]))
    for button in ("left", "back"):
        asserts.equals(env, 'script_args name "%s" conflicts with a board Button' % button, lua_web_app_board_argument_error([button], ["left", "back"]))
    return unittest.end(env)

lua_web_app_argument_test = unittest.make(_argument_errors_impl)
