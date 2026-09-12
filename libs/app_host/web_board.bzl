"""Web boards: the browser counterpart of a physical board.

A web board declares the peripherals a browser page offers (display size and
push-edge Buttons with their keyboard keys) and one or more skins, each an
HTML/CSS page that looks like the device. h2_web_app() assembles a runnable
page from an App, one board and one of its skins; the App never carries its
own key table or layout.
"""

H2WebBoardInfo = provider(
    doc = "Peripherals and skins of one web board.",
    fields = {
        "display_width": "Display width in pixels.",
        "display_height": "Display height in pixels.",
        "button_names": "Ordered Button names; Button i has periph id i + 1.",
        "button_keys": "KeyboardEvent.key for each Button, \"\" for none.",
        "skins": "Dict of skin name to its HTML File.",
        "default_skin": "Skin used when a target selects none, or \"\".",
    },
)

_NAME_CHARS = "abcdefghijklmnopqrstuvwxyz0123456789_"
_MAX_BUTTONS = 8  # H2_WEB_APP_HOST_MAX_BUTTONS

def web_board_argument_error(display_width, display_height, buttons, skins, default_skin):
    """Returns why h2_web_board() arguments are invalid, or "" when valid."""
    for value in (display_width, display_height):
        if type(value) != "int" or value < 1 or value > 4096:
            return "display size %r must be an int in 1..4096" % value
    if len(buttons) > _MAX_BUTTONS:
        return "buttons needs at most %d entries" % _MAX_BUTTONS
    for button in buttons.keys():
        if not button or [c for c in button.elems() if c not in _NAME_CHARS]:
            return "Button name %r must use [a-z0-9_]" % button
    for skin in skins.keys():
        if not skin or [c for c in skin.elems() if c not in _NAME_CHARS]:
            return "skin name %r must use [a-z0-9_]" % skin
    if default_skin and default_skin not in skins:
        return "default_skin %r is not in skins" % default_skin
    return ""

def _web_board_impl(ctx):
    return [H2WebBoardInfo(
        display_width = ctx.attr.display_width,
        display_height = ctx.attr.display_height,
        button_names = ctx.attr.button_names,
        button_keys = ctx.attr.button_keys,
        skins = {name: target.files.to_list()[0] for target, name in ctx.attr.skins.items()},
        default_skin = ctx.attr.default_skin,
    )]

_web_board = rule(
    implementation = _web_board_impl,
    attrs = {
        "button_keys": attr.string_list(),
        "button_names": attr.string_list(),
        "default_skin": attr.string(),
        "display_height": attr.int(mandatory = True),
        "display_width": attr.int(mandatory = True),
        "skins": attr.label_keyed_string_dict(allow_files = [".html"]),
    },
)

def h2_web_board(
        name,
        display_width,
        display_height,
        buttons = {},
        skins = {},
        default_skin = None,
        visibility = None):
    """Declares a web board.

    Args:
      name: Board target name.
      display_width: Display (canvas) width in pixels.
      display_height: Display (canvas) height in pixels.
      buttons: Ordered dict of Button name ([a-z0-9_]) to the keyboard key
        (KeyboardEvent.key, "" for none) that drives it; at most 8. Button i
        is periph id i + 1. Skin elements marked data-h2-button="<name>"
        drive the same Button.
      skins: Dict of skin name ([a-z0-9_]) to an HTML file (markup plus
        <style>) that looks like the device. app_host places the canvas,
        Start/Stop controls and status line in its data-h2-slot="display" |
        "controls" | "status" elements.
      default_skin: Skin used when a target selects none. Without skins the
        page uses //libs/app_host:default_layout.html.
      visibility: Target visibility.
    """
    error = web_board_argument_error(
        display_width,
        display_height,
        buttons,
        skins,
        default_skin,
    )
    if error:
        fail("h2_web_board: " + error)
    _web_board(
        name = name,
        display_width = display_width,
        display_height = display_height,
        button_names = list(buttons.keys()),
        button_keys = list(buttons.values()),
        skins = {label: skin for skin, label in skins.items()},
        default_skin = default_skin or "",
        visibility = visibility,
    )
