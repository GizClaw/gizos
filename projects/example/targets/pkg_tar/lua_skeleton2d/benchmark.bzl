"""Independent browser invocations covering the complete skeleton benchmark."""
load("//libs/lua/web:lua_web_app.bzl", "h2_lua_web_app")
load("//tools/bazel:web_archive.bzl", "web_archive_browser_test")

def skeleton_benchmark_group(group):
    h2_lua_web_app(
        name = "benchmark",
        board = "//projects/example/libs/web/demo_board",
        skin = "plain",
        script = "//projects/example/apps/lua_skeleton2d/app:src/skeleton2d.lua",
        script_args = {"benchmark_group": str(group)},
        vm_memory_limit_bytes = 2097152,
        clicks = [["SKELETON FRAME initial", "#stop"]],
    )
    web_archive_browser_test(
        name = "benchmark_browser_test",
        archive = ":benchmark",
        passes = ["SKELETON BENCH COMPLETE group=%d" % group,
                  "H2_WEB_APP name=benchmark result=PASS"] + (
            ["SKELETON BENCH tier=3 run=3", "SKELETON BENCH tier=1 run=3 case=8"]
            if group == 1 else ["SKELETON SPATIAL COMPLETE", "SKELETON SPATIAL .*run=3"]
        ),
        fails = ["SKELETON BENCH SKIP"],
        taps = [["SKELETON FRAME initial", 216, 230]],
        clicks = [["SKELETON BENCH COMPLETE", "#stop"]],
        timeout_s = 2400,
        size = "enormous",
    )
