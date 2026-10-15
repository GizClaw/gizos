"""Analysis tests for h2_web_board() argument validation."""

load("@bazel_skylib//lib:unittest.bzl", "asserts", "unittest")
load(":web_board.bzl", "web_board_argument_error")

_BUTTONS = {"left": "ArrowLeft", "ok": "Enter", "back": "Escape"}
_SKINS = {"device": "device.html", "plain": "plain.html"}

def _argument_errors_impl(ctx):
    env = unittest.begin(ctx)
    asserts.equals(env, "", web_board_argument_error(240, 240, _BUTTONS, _SKINS, "device"))
    asserts.equals(env, "", web_board_argument_error(240, 240, {}, {}, None))
    asserts.equals(
        env,
        "display size 0 must be an int in 1..4096",
        web_board_argument_error(0, 240, _BUTTONS, _SKINS, None),
    )
    asserts.equals(
        env,
        'display size "240" must be an int in 1..4096',
        web_board_argument_error(240, "240", _BUTTONS, _SKINS, None),
    )
    asserts.equals(
        env,
        "buttons needs at most 8 entries",
        web_board_argument_error(240, 240, {"b%d" % i: "" for i in range(9)}, {}, None),
    )
    asserts.equals(
        env,
        'Button name "Back" must use [a-z0-9_]',
        web_board_argument_error(240, 240, {"Back": "Escape"}, {}, None),
    )
    asserts.equals(
        env,
        'Button name "" must use [a-z0-9_]',
        web_board_argument_error(240, 240, {"": "Escape"}, {}, None),
    )
    asserts.equals(
        env,
        'skin name "Device" must use [a-z0-9_]',
        web_board_argument_error(240, 240, _BUTTONS, {"Device": "d.html"}, None),
    )
    asserts.equals(
        env,
        'default_skin "round" is not in skins',
        web_board_argument_error(240, 240, _BUTTONS, _SKINS, "round"),
    )
    return unittest.end(env)

web_board_argument_test = unittest.make(_argument_errors_impl)
