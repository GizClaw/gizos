if(NOT DEFINED H2_REPO_ROOT)
  get_filename_component(H2_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)
else()
  get_filename_component(H2_REPO_ROOT "${H2_REPO_ROOT}" ABSOLUTE)
endif()

include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_MAIN_SOURCES main)

idf_component_register(
  SRCS ${H2_MAIN_SOURCES}
  INCLUDE_DIRS
    "${H2_REPO_ROOT}/projects/h2loader/libs/h2loader/include"
    "${H2_REPO_ROOT}/libs/pal/include"
    "${H2_REPO_ROOT}/libs/bundle/include"
  REQUIRES
    h2_esp_target_task_policy
    espcoredump
    h2_firmware_lib
    h2_esp_board
    h2_h2loader_ble
    h2_h2loader_runtime
    h2_pal_core
    h2_firmware_lib
)

idf_component_get_property(
  H2_AMOLED_MFG_BOARD_COMPONENT h2_esp_board COMPONENT_LIB)
target_compile_definitions(
  ${H2_AMOLED_MFG_BOARD_COMPONENT} PRIVATE H2_AMOLED_BOARD_ENABLE_QMI8658=1)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu11 -Wall -Wextra -Werror)
