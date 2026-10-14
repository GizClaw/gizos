"""A selected BSP's complete default PAL Application Host provider."""

load("//tools/bazel:native_component.bzl", "firmware_native_component")

def board_application_host(name, board, sdk):
    prefix = "esp" if sdk == "esp" else "bk"
    directory = "boards/shared/application_host/" + sdk
    firmware_native_component(
        name = name,
        srcs = ["//" + directory + ":src/h2_" + prefix + "_board_application_host.c"],
        hdrs = ["//" + directory + ":include/h2_" + prefix + "_board_application_host.h"],
        data = ["//" + directory + ":CMakeLists.txt"],
        component_directory = directory,
        component_name = "h2_board_application_host",
        execution_unit = "" if sdk == "esp" else "ap",
        deps = [board, "//libs/pal", "//native_component_src/" + ("esp-idf6.x" if sdk == "esp" else "bk7258/ap") + "/h2_application_host"],
    )
