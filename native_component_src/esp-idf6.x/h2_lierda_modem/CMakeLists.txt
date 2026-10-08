set(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")
include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_COMPONENT_SOURCES h2_lierda_modem)
idf_component_register(
  SRCS ${H2_COMPONENT_SOURCES}
  INCLUDE_DIRS "include"
    "${REPO_ROOT}/libs/pal/providers/modem/lierda/include"
    "${REPO_ROOT}/libs/pal/include"
  REQUIRES h2_firmware_lib esp_driver_uart esp_driver_gpio esp_event esp_modem esp_netif h2_pal_core)
target_compile_options(${COMPONENT_LIB} PRIVATE
  "$<$<COMPILE_LANGUAGE:C>:-std=gnu11>" -Wall -Wextra -Werror)
set_target_properties(${COMPONENT_LIB} PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS ON)
