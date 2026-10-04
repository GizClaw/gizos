cmake_minimum_required(VERSION 3.5)

if(DEFINED ENV{H2_REPO_ROOT} AND NOT "$ENV{H2_REPO_ROOT}" STREQUAL "")
  set(H2_REPO_ROOT "$ENV{H2_REPO_ROOT}")
else()
  get_filename_component(H2_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../.." ABSOLUTE)
endif()

set(H2_BK_TARGET "bk7258")
include($ENV{ARMINO_TOOLS_PATH}/build_tools/cmake/project.cmake)
include("$ENV{H2_BAZEL_COMPONENT_MANIFEST}")

if("${ARMINO_SOC}" STREQUAL "${H2_BK_TARGET}_ap")
  # Select the existing real provider only for this standalone AP artifact.
  # Component directories inherit this project scope when project() configures
  # them; the reusable PAL/BSP default and CP build remain unchanged.
  set(H2_BK_PLATFORM_CORE_ENABLE_MQTT ON)
  foreach(H2_COMPONENT_DIR IN LISTS H2_BAZEL_AP_COMPONENT_DIRS)
    armino_build_component("${H2_COMPONENT_DIR}")
  endforeach()
elseif("${ARMINO_SOC}" STREQUAL "${H2_BK_TARGET}")
  foreach(H2_COMPONENT_DIR IN LISTS H2_BAZEL_CP_COMPONENT_DIRS)
    armino_build_component("${H2_COMPONENT_DIR}")
  endforeach()
else()
  message(FATAL_ERROR "Unsupported BK7258 execution unit: ${ARMINO_SOC}")
endif()

project(h2_bk_pal_mqtt)

if("${ARMINO_SOC}" STREQUAL "${H2_BK_TARGET}_ap")
  target_compile_definitions(__armino_h2_pal_core PRIVATE H2_BK_NET_TLS_DIAGNOSTICS=1)
  get_target_property(H2_MQTT_PLATFORM_DEFINITIONS
    __armino_h2_pal_core COMPILE_DEFINITIONS)
  list(FIND H2_MQTT_PLATFORM_DEFINITIONS
    "H2_BK_PLATFORM_CORE_ENABLE_MQTT=1" H2_MQTT_ENABLED_INDEX)
  list(FIND H2_MQTT_PLATFORM_DEFINITIONS
    "H2_BK_PLATFORM_CORE_ENABLE_MQTT=0" H2_MQTT_DISABLED_INDEX)
  if(H2_MQTT_ENABLED_INDEX LESS 0 OR NOT H2_MQTT_DISABLED_INDEX LESS 0)
    message(FATAL_ERROR "PAL MQTT AP artifact did not enable the real platform provider")
  endif()
  # This translation unit is compiled with the provider's actual private flags,
  # rather than trusting a launcher-only macro or an unpropagated CMake option.
  set(H2_MQTT_COMPILE_ASSERT "${CMAKE_BINARY_DIR}/h2_mqtt_provider_enabled.c")
  file(WRITE "${H2_MQTT_COMPILE_ASSERT}"
    "#if !defined(H2_BK_PLATFORM_CORE_ENABLE_MQTT) || H2_BK_PLATFORM_CORE_ENABLE_MQTT != 1\n#error PAL MQTT provider must be enabled in its actual compilation unit\n#endif\nvoid h2_mqtt_provider_compile_assert(void) {}\n")
  target_sources(__armino_h2_pal_core PRIVATE "${H2_MQTT_COMPILE_ASSERT}")
  message(STATUS
    "H2_PAL_MQTT_COMPILE_POLICY target=__armino_h2_pal_core H2_BK_PLATFORM_CORE_ENABLE_MQTT=1 outgoing_records=8 incoming_records=8")
endif()
