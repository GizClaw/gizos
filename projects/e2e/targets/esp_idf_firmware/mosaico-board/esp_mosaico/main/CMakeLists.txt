include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_MAIN_SOURCES main)
idf_component_register(SRCS ${H2_MAIN_SOURCES}
  INCLUDE_DIRS "." "$ENV{H2_GIZOS_ROOT}/projects/e2e/apps/pal-audio/app/include" "$ENV{H2_GIZOS_ROOT}/projects/e2e/apps/pal/app/include"
  REQUIRES h2_mosaico_camera esp_mosaico_bsp espressif__esp_lcd_touch h2_esp_target_task_policy h2_firmware_lib h2_esp_board h2_mosaico_usb_console h2_chip_board)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu11 -Wall -Wextra -Werror)
