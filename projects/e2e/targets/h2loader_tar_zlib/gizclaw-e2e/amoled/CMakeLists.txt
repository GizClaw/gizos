cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED H2_REPO_ROOT)
  get_filename_component(H2_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../.." ABSOLUTE)
else()
  get_filename_component(H2_REPO_ROOT "${H2_REPO_ROOT}" ABSOLUTE)
endif()

set(H2_ESP_TARGET "esp32s3")
set(H2_ESP_BOARD_DIR "${H2_REPO_ROOT}/boards/amoled/${H2_ESP_TARGET}" CACHE PATH "H2 ESP board component directory" FORCE)
set(H2_H2LOADER_COMPONENT_DIR "${H2_REPO_ROOT}/projects/h2loader/native_component_src/esp-idf6.x")

if(NOT DEFINED IDF_TARGET)
  set(IDF_TARGET "${H2_ESP_TARGET}" CACHE STRING "ESP-IDF target")
endif()
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig" CACHE FILEPATH "ESP-IDF sdkconfig" FORCE)
set(SDKCONFIG_DEFAULTS
  "${H2_ESP_BOARD_DIR}/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.h2loader.defaults"
  CACHE STRING "ESP-IDF sdkconfig defaults" FORCE)

set(H2_ESP_PLATFORM_ENABLE_WEBRTC ON CACHE BOOL
  "Enable the H2Peer-backed ESP WebRTC provider for GizClaw E2E" FORCE)

set(H2_ESP_BOARD_ALIAS_DIR "${CMAKE_BINARY_DIR}/h2_esp_board")
file(REMOVE_RECURSE "${H2_ESP_BOARD_ALIAS_DIR}")
file(CREATE_LINK "${H2_ESP_BOARD_DIR}" "${H2_ESP_BOARD_ALIAS_DIR}" SYMBOLIC)

include("$ENV{H2_BAZEL_COMPONENT_MANIFEST}")
set(EXTRA_COMPONENT_DIRS ${H2_BAZEL_COMPONENT_DIRS})
set(COMPONENTS ${H2_BAZEL_COMPONENT_NAMES})

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(h2_gizclaw_e2e_amoled)
