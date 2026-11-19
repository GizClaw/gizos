cmake_minimum_required(VERSION 3.16)

set(H2_REPO_ROOT "$ENV{H2_GIZOS_ROOT}")
set(H2_ESP_TARGET "esp32s3")
set(H2_ESP_BOARD_DIR "${H2_REPO_ROOT}/boards/haivivi/h106_tiga_esp_v4_2/${H2_ESP_TARGET}")
set(IDF_TARGET "${H2_ESP_TARGET}" CACHE STRING "ESP-IDF target")
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig" CACHE FILEPATH "ESP-IDF sdkconfig" FORCE)
set(SDKCONFIG_DEFAULTS
  "${H2_ESP_BOARD_DIR}/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.h2loader.defaults"
  CACHE STRING "ESP-IDF sdkconfig defaults" FORCE)

include("$ENV{H2_BAZEL_COMPONENT_MANIFEST}")
set(EXTRA_COMPONENT_DIRS ${H2_BAZEL_COMPONENT_DIRS})
set(COMPONENTS ${H2_BAZEL_COMPONENT_NAMES})

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(h2_display_bench_tiga)
