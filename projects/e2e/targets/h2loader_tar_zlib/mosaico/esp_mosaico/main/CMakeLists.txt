include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_MAIN_SOURCES main)
idf_component_register(SRCS ${H2_MAIN_SOURCES}
  INCLUDE_DIRS "."
  REQUIRES h2_esp_target_task_policy h2_firmware_lib h2_esp_board h2_mosaico_loader_usb
    h2_h2loader_runtime h2_h2loader_ble h2_pal_core esp_timer app_update espcoredump)
target_compile_definitions(${COMPONENT_LIB} PRIVATE H2_E2E_MOSAICO=1)
if(DEFINED H2_MOSAICO_CORE_WALL_UTC_MS)
  string(LENGTH "${H2_MOSAICO_CORE_WALL_UTC_MS}" H2_WALL_FIXTURE_LENGTH)
  if(NOT H2_MOSAICO_CORE_WALL_UTC_MS MATCHES "^[1-9][0-9]*$" OR
     NOT H2_WALL_FIXTURE_LENGTH EQUAL 13 OR
     H2_MOSAICO_CORE_WALL_UTC_MS LESS 1577836800000 OR
     H2_MOSAICO_CORE_WALL_UTC_MS GREATER 4102444800000)
    message(FATAL_ERROR "H2_MOSAICO_CORE_WALL_UTC_MS must be explicit UTC Unix milliseconds")
  endif()
  target_compile_definitions(${COMPONENT_LIB} PRIVATE
    H2_MOSAICO_CORE_WALL_UTC_MS=${H2_MOSAICO_CORE_WALL_UTC_MS}ULL)
endif()
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu11 -Wall -Wextra -Werror)

# The libco-only failure diagnostic reads bounds from the pinned SDK private API.
target_include_directories(${COMPONENT_LIB} PRIVATE
  "$ENV{IDF_PATH}/components/esp_system/port/include/private")
