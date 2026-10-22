include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_MAIN_SOURCES main)
idf_component_register(SRCS ${H2_MAIN_SOURCES}
  INCLUDE_DIRS "."
  REQUIRES h2_esp_target_task_policy h2_firmware_lib h2_esp_board h2_mosaico_loader_usb
    h2_h2loader_runtime h2_h2loader_ble h2_pal_core esp_timer app_update espcoredump)
target_compile_definitions(${COMPONENT_LIB} PRIVATE H2_E2E_MOSAICO=1)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu11 -Wall -Wextra -Werror)
