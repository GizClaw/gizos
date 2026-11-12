"""G99 Web packages: the portable Main, or its product E2E suite, on the Web PAL."""

load("//tools/bazel/web:web_app.bzl", "h2_web_app")
load("//tools/bazel/web:web_archive.bzl", "web_archive_browser_test")

_WEB = "//projects/g99/targets/pkg_tar/web"

def g99_web(name, board, styles = [], external_storage = "spi_flash"):
    """Declares the archive `<name>`, `serve`, `browser_test` and `navigation_test`.

    Main embeds its font and the Orbit app, so the page preloads no files.
    Settings persist in the Web PAL pref (the page origin's localStorage).

    Args:
      name: Archive target name; the tar is `<name>.web.tar`.
      board: h2_web_board() target with Buttons home, screen, up, down and ok.
      styles: Product-owned CSS applied after the board skin.
      external_storage: Preview storage medium, "nand" or "spi_flash".
    """
    if external_storage not in ["nand", "spi_flash"]:
        fail("unknown G99 external storage kind: %s" % external_storage)
    passed = "H2_WEB_APP name=%s result=PASS rc=0 destroy=0" % name
    failed = "H2_WEB_APP name=%s result=FAIL" % name
    h2_web_app(
        name = name,
        srcs = [
            _WEB + ":main.c",
            _WEB + ":web_buttons.c",
            _WEB + ":web_buttons.h",
        ],
        app_name = name,
        board = board,
        styles = styles,
        copts = [
            r'-DH2_G99_WEB_NAME=\"%s\"' % name,
            "-DH2_G99_PREVIEW_NAND=" + ("1" if external_storage == "nand" else "0"),
        ],
        linkopts = [
            "-O2",
            "-sMINIFY_HTML=0",
        ],
        # Main must reach its menu and then stop cleanly on the page's Stop.
        passes = [passed],
        clicks = [["H2_G99_WEB arrival=1 route=main", "#stop"]],
        canvas_min = 1000,
        deps = [
            "//projects/g99/apps/main:main",
            "//libs/runtime",
        ],
    )

    # Boot ends on MAIN(Library) no earlier than the 900 ms animation, OK
    # enters Library, AI (Escape) returns to MAIN(Library), then Stop.
    web_archive_browser_test(
        name = "navigation_test",
        archive = ":" + name,
        presses = [
            ["H2_G99_WEB arrival=1 route=main main=2 ", "Enter"],
            ["H2_G99_WEB arrival=2 route=library main=2 sub=0 ", "Escape"],
        ],
        clicks = [["H2_G99_WEB arrival=3 route=main main=2 ", "#stop"]],
        passes = [
            "H2_G99_WEB arrival=1 route=main main=2 sub=0 t=(9\\d\\d|[1-9]\\d{3,})\\b",
            "H2_G99_WEB arrival=2 route=library main=2 sub=0 ",
            "H2_G99_WEB arrival=3 route=main main=2 sub=0 ",
            passed,
        ],
        fails = [
            failed,
            "H2_G99_WEB input_failed",
            "H2_G99_WEB arrival=[0-9]+ route=(app|settings)",
        ],
        canvas_min = 1000,
        timeout_s = 90,
    )

def g99_web_e2e(name, board, styles = []):
    """Declares the E2E archive `<name>`, `serve` and its `browser_test`.

    The page runs the G99 product E2E suite (//projects/g99/apps/e2e)
    against the production Main and passes when the suite's summary reports a
    complete run with exit code 0. `?select=<selection>` on the page URL picks
    cases; under Bazel pass `--test_arg=--query=select=<selection>`.

    Args:
      name: Archive target name; the tar is `<name>.web.tar`.
      board: h2_web_board() target; its display size is the size under test.
      styles: Product-owned CSS applied after the board skin.
    """
    passed = "H2_WEB_APP name=%s result=PASS rc=0 destroy=0" % name
    h2_web_app(
        name = name,
        srcs = [
            _WEB + ":main.c",
            _WEB + ":web_buttons.c",
            _WEB + ":web_buttons.h",
        ],
        app_name = name,
        board = board,
        styles = styles,
        copts = [
            r'-DH2_G99_WEB_NAME=\"%s\"' % name,
            "-DH2_G99_WEB_E2E=1",
        ],
        linkopts = [
            "-O2",
            "-sMINIFY_HTML=0",
        ],
        passes = [
            "H2_E2E_SUMMARY .* complete=true .*exit_code=0 ",
            passed,
        ],
        fails = [
            "H2_E2E_CASE .* status=(FAIL|ERROR) ",
            "H2_E2E_SUMMARY .* exit_code=[12] ",
            "H2_WEB_APP name=%s result=FAIL" % name,
            "H2_G99_WEB input_failed",
        ],
        test_timeout_s = 240,
        deps = [
            "//projects/g99/apps/e2e:e2e",
            "//projects/g99/apps/main:main",
            "//libs/runtime",
        ],
    )
