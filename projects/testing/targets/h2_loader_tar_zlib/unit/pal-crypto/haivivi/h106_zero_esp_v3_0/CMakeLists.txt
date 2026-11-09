cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED H2_REPO_ROOT)
  get_filename_component(H2_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../../../.." ABSOLUTE)
endif()
set(H2_ESP_TARGET "esp32s3")
set(H2_ESP_BOARD_DIR "${H2_REPO_ROOT}/boards/haivivi/h106_zero_esp_v3_0/${H2_ESP_TARGET}")
set(H2_ESP_COMPONENT_DIR "${H2_REPO_ROOT}/native_component_src/esp-idf6.x")
set(IDF_TARGET "esp32s3" CACHE STRING "ESP-IDF target")
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig" CACHE FILEPATH "ESP-IDF sdkconfig" FORCE)
set(SDKCONFIG_DEFAULTS
  "${H2_ESP_BOARD_DIR}/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.h2loader.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.pal-crypto.defaults"
  CACHE STRING "ESP-IDF sdkconfig defaults" FORCE)

include("$ENV{H2_BAZEL_COMPONENT_MANIFEST}")
set(EXTRA_COMPONENT_DIRS ${H2_BAZEL_COMPONENT_DIRS})
set(COMPONENTS ${H2_BAZEL_COMPONENT_NAMES})
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(h2_zero_esp_pal_crypto)
