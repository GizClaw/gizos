if(DEFINED H2_ESP_BOARD_DIR)
  get_filename_component(H2_REPO_ROOT "${H2_ESP_BOARD_DIR}/../../.." ABSOLUTE)
else()
  get_filename_component(H2_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
endif()

set(H2_ESP_BOARD_INCLUDE_DIRS
  "include"
  "${H2_REPO_ROOT}/libs/pal/include"
  "${H2_REPO_ROOT}/libs/runtime/include")

set(H2_ESP_BOARD_REQUIRES
  esp_driver_gpio
  esp_driver_tsens
  h2_pal_core
  h2_firmware_lib)

include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_BOARD_SOURCES h2_esp_board)

idf_component_register(
  SRCS ${H2_BOARD_SOURCES}
  INCLUDE_DIRS
    ${H2_ESP_BOARD_INCLUDE_DIRS}
  REQUIRES
    ${H2_ESP_BOARD_REQUIRES})

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
