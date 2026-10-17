cmake_minimum_required(VERSION 3.16)

foreach(required H2_REPO_ROOT H2_E2E_BOARD_DIR H2_E2E_PROJECT_NAME)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "Bazel target did not select ${required}")
  endif()
endforeach()

set(H2_ESP_TARGET "esp32s3")
set(H2_ESP_BOARD_DIR "${H2_REPO_ROOT}/${H2_E2E_BOARD_DIR}")
set(IDF_TARGET "${H2_ESP_TARGET}" CACHE STRING "ESP-IDF target")
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig" CACHE FILEPATH "ESP-IDF sdkconfig" FORCE)
set(SDKCONFIG_DEFAULTS "${H2_ESP_BOARD_DIR}/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.h2loader.defaults")
if(H2_E2E_SUITE_DEFAULTS)
  list(APPEND SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/${H2_E2E_SUITE_DEFAULTS}")
endif()
set(H2_ESP_BOARD_ENABLE_RUNTIME ON CACHE BOOL "Enable board Runtime provider" FORCE)

include("$ENV{H2_BAZEL_COMPONENT_MANIFEST}")
set(EXTRA_COMPONENT_DIRS ${H2_BAZEL_COMPONENT_DIRS})
set(COMPONENTS ${H2_BAZEL_COMPONENT_NAMES})

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(${H2_E2E_PROJECT_NAME})
