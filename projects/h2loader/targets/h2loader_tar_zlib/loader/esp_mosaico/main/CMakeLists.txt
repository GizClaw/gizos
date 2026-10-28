include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_MAIN_SOURCES main)
idf_component_register(SRCS ${H2_MAIN_SOURCES} REQUIRES h2_esp_target_task_policy h2_esp_board h2_mosaico_loader_usb h2_h2loader_runtime h2_firmware_lib)
