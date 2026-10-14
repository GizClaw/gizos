"""Shared native PAL E2E launcher selected with one board dependency."""

load("//tools/bazel:native_component.bzl", "firmware_native_component")

def pal_e2e_application_host(name, board, sdk):
    directory = "projects/e2e/libs/application-host/" + sdk
    firmware_native_component(
        name = name,
        srcs = ["//projects/e2e/libs/application-host:src/h2_pal_e2e_application_host.c"],
        hdrs = ["//projects/e2e/libs/application-host:include/h2_pal_e2e_application_host.h"],
        data = ["//" + directory + ":CMakeLists.txt"],
        component_directory = directory,
        component_name = "h2_pal_e2e_application_host",
        execution_unit = "" if sdk == "esp" else "ap",
        deps = [board, "//libs/pal/application/host", "//native_component_src/" + ("esp-idf6.x" if sdk == "esp" else "bk7258/ap") + "/h2_pal_core", "//projects/h2loader/native_component_src/" + ("esp-idf6.x/h2_h2loader_ble" if sdk == "esp" else "bk7258/ap/h2loader_bk")],
    )
