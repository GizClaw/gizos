set(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")

include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_COMPONENT_SOURCES h2_simcom_modem)

idf_component_register(
  SRCS ${H2_COMPONENT_SOURCES}
  INCLUDE_DIRS
    "include"
    "${REPO_ROOT}/libs/pal/providers/modem/simcom/include"
    "${REPO_ROOT}/libs/pal/include"
  PRIV_INCLUDE_DIRS
    "${REPO_ROOT}/libs/pal/providers/modem/simcom/src"
  REQUIRES
    h2_firmware_lib
    esp_driver_gpio
    esp_driver_uart
    esp_event
    esp_modem
    esp_netif
    h2_pal_core)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu11 -Wall -Wextra -Werror)

if(CMAKE_BUILD_EARLY_EXPANSION)
  return()
endif()
