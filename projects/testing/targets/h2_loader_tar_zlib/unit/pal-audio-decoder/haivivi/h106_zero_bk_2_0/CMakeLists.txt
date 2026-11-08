cmake_minimum_required(VERSION 3.5)

set(H2_BK_TARGET "bk7258")
include($ENV{ARMINO_TOOLS_PATH}/build_tools/cmake/project.cmake)
include("$ENV{H2_BAZEL_COMPONENT_MANIFEST}")

if("${ARMINO_SOC}" STREQUAL "${H2_BK_TARGET}_ap")
  foreach(H2_COMPONENT_DIR IN LISTS H2_BAZEL_AP_COMPONENT_DIRS)
    armino_build_component("${H2_COMPONENT_DIR}")
  endforeach()
elseif("${ARMINO_SOC}" STREQUAL "${H2_BK_TARGET}")
  foreach(H2_COMPONENT_DIR IN LISTS H2_BAZEL_CP_COMPONENT_DIRS)
    armino_build_component("${H2_COMPONENT_DIR}")
  endforeach()
else()
  message(FATAL_ERROR "Unsupported H106 BK execution unit: ${ARMINO_SOC}")
endif()

project(h2_zero_bk_pal_audio_decoder)
