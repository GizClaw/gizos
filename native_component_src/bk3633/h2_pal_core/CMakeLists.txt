# BK3633 chip PAL backend.
#
# This component deliberately has no board-specific include path. A launcher
# supplies the selected board/flavor include directories and the SDK root.

cmake_minimum_required(VERSION 3.16)
project(bk3633_h2_pal_core C)

get_filename_component(FIRMWARES_ROOT
  "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

set(BK3633_PATH "$ENV{BK3633_PATH}" CACHE PATH
    "BK3633 repository root containing SDK/")
set(BK3633_CONFIG_INCLUDE_DIR "" CACHE PATH
    "Board/flavor include directory containing rwip_config.h")
if(NOT BK3633_PATH)
  message(FATAL_ERROR
    "BK3633_PATH is required and must point to the BK3633 repository root")
endif()
set(BK3633_SDK_ROOT "${BK3633_PATH}/SDK")
if(NOT EXISTS "${BK3633_SDK_ROOT}/src")
  message(FATAL_ERROR "BK3633_PATH must contain SDK/src/")
endif()

set(H2_REPOSITORY_ROOT "${FIRMWARES_ROOT}")
set(H2_LIBCO_ARCHIVE "$ENV{H2_BAZEL_PREBUILT_H2_LIBCO}" CACHE FILEPATH
    "Bazel-built //libs/pal/providers/libco:libco archive")
if(NOT H2_LIBCO_ARCHIVE OR NOT EXISTS "${H2_LIBCO_ARCHIVE}")
  message(FATAL_ERROR
    "H2_BAZEL_PREBUILT_H2_LIBCO must name the Bazel-built libco archive")
endif()
add_library(h2_firmware_lib STATIC IMPORTED GLOBAL)
set_target_properties(h2_firmware_lib PROPERTIES
  IMPORTED_LOCATION "${H2_LIBCO_ARCHIVE}"
  INTERFACE_INCLUDE_DIRECTORIES
    "${FIRMWARES_ROOT}/libs/pal/providers/libco/include;${FIRMWARES_ROOT}/libs/pal/include")
if(NOT BK3633_CONFIG_INCLUDE_DIR OR
   NOT EXISTS "${BK3633_CONFIG_INCLUDE_DIR}/rwip_config.h")
  message(FATAL_ERROR
    "BK3633_CONFIG_INCLUDE_DIR must contain rwip_config.h")
endif()

set(H2_BK3633_FIRMWARE_VERSION "" CACHE STRING
    "Explicit version embedded in the BK3633 firmware-info PAL provider")
if(NOT H2_BK3633_FIRMWARE_VERSION STREQUAL "")
  set(H2_BK3633_EFFECTIVE_FIRMWARE_VERSION
      "${H2_BK3633_FIRMWARE_VERSION}")
elseif(NOT "$ENV{H2_FIRMWARE_VERSION}" STREQUAL "")
  set(H2_BK3633_EFFECTIVE_FIRMWARE_VERSION
      "$ENV{H2_FIRMWARE_VERSION}")
else()
  execute_process(
    COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${FIRMWARES_ROOT}"
    OUTPUT_VARIABLE H2_BK3633_EFFECTIVE_FIRMWARE_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE H2_BK3633_FIRMWARE_VERSION_RESULT)
  if(NOT H2_BK3633_FIRMWARE_VERSION_RESULT EQUAL 0 OR
     H2_BK3633_EFFECTIVE_FIRMWARE_VERSION STREQUAL "")
    message(FATAL_ERROR "failed to derive BK3633 firmware version")
  endif()
endif()

set(BK3633_SDK_INCLUDE_DIRS
  "${BK3633_SDK_ROOT}/src"
  "${BK3633_SDK_ROOT}/src/header"
  "${BK3633_SDK_ROOT}/src/header/header_file"
  "${BK3633_SDK_ROOT}/src/ip/ble/hl/api"
  "${BK3633_SDK_ROOT}/src/ip/ble/hl/inc"
  "${BK3633_SDK_ROOT}/src/ip/ble/ll/api"
  "${BK3633_SDK_ROOT}/src/ip/ble/ll/import/reg"
  "${BK3633_SDK_ROOT}/src/ip/ble/ll/src"
  "${BK3633_SDK_ROOT}/src/ip/hci/api"
  "${BK3633_SDK_ROOT}/src/ip/sch/api"
  "${BK3633_SDK_ROOT}/src/ip/sch/import"
  "${BK3633_SDK_ROOT}/src/modules/common/api"
  "${BK3633_SDK_ROOT}/src/modules/ke/api"
  "${BK3633_SDK_ROOT}/src/plf/nvds/api"
  "${BK3633_SDK_ROOT}/src/plf/rwip/api"
  "${BK3633_SDK_ROOT}/src/plf/rwip/import/reg"
  "${BK3633_SDK_ROOT}/src/plf/prf"
  "${BK3633_SDK_ROOT}/src/app/api"
  "${BK3633_SDK_ROOT}/src/profiles/bas/bass/api"
  "${BK3633_SDK_ROOT}/src/profiles/dis/diss/api"
  "${BK3633_SDK_ROOT}/src/profiles/oad/api"
  "${BK3633_SDK_ROOT}/src/plf/refip/import/reg"
  "${BK3633_SDK_ROOT}/src/plf/refip/src/arch"
  "${BK3633_SDK_ROOT}/src/plf/refip/src/arch/compiler/armgcc"
  "${BK3633_SDK_ROOT}/src/plf/refip/src/arch/ll/armgcc"
  "${BK3633_SDK_ROOT}/src/driver/flash"
  "${BK3633_SDK_ROOT}/src/driver/adc"
  "${BK3633_SDK_ROOT}/src/driver/i2c"
  "${BK3633_SDK_ROOT}/src/driver/icu"
  "${BK3633_SDK_ROOT}/src/driver/intc"
  "${BK3633_SDK_ROOT}/src/driver/gpio"
  "${BK3633_SDK_ROOT}/src/driver/pwm"
  "${BK3633_SDK_ROOT}/src/driver/timer0"
  "${BK3633_SDK_ROOT}/src/driver/uart"
  "${BK3633_SDK_ROOT}/src/driver/uart2"
)

add_library(bk3633_h2_pal_core STATIC
  "${FIRMWARES_ROOT}/libs/drivers/rtc/bm8563/src/h2_bm8563.c"
  src/h2_bk3633_flash_disk.c
  src/h2_bk3633_nvds_pref.c
  src/h2_bk3633_platform_ble.c
  src/h2_bk3633_platform_battery.c
  src/h2_bk3633_platform_button.c
  src/h2_bk3633_platform_buzzer.c
  src/h2_bk3633_platform_entropy.c
  src/h2_bk3633_platform_firmware_info.c
  src/h2_bk3633_platform_log.c
  src/h2_bk3633_platform_libco.c
  src/h2_bk3633_platform_mem.c
  src/h2_bk3633_platform_power.c
  src/h2_bk3633_platform_system_event.c
  src/h2_bk3633_platform_time.c
)

target_include_directories(bk3633_h2_pal_core
  PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${FIRMWARES_ROOT}/libs/drivers/rtc/bm8563/include"
    "${FIRMWARES_ROOT}/libs/pal/include"
  PRIVATE
    "${BK3633_CONFIG_INCLUDE_DIR}"
    ${BK3633_SDK_INCLUDE_DIRS}
)

# Consumers add their board/flavor include directory and SDK-native source
# manifest. No board is selected here.
target_compile_features(bk3633_h2_pal_core PRIVATE c_std_11)
target_compile_definitions(bk3633_h2_pal_core PRIVATE
  BK3633
  CFG_CPU_BK3633
  H2_BK3633_FIRMWARE_VERSION="${H2_BK3633_EFFECTIVE_FIRMWARE_VERSION}")
target_link_libraries(bk3633_h2_pal_core PUBLIC h2_firmware_lib)

add_executable(bk3633_sdk_probe EXCLUDE_FROM_ALL probe/sdk_probe.c)
target_include_directories(bk3633_sdk_probe PRIVATE
  "${BK3633_CONFIG_INCLUDE_DIR}" ${BK3633_SDK_INCLUDE_DIRS})

add_executable(bk3633_ble_probe EXCLUDE_FROM_ALL probes/src/ble_probe.c)
target_include_directories(bk3633_ble_probe PRIVATE
  "${BK3633_CONFIG_INCLUDE_DIR}" ${BK3633_SDK_INCLUDE_DIRS})

add_executable(bk3633_entropy_probe EXCLUDE_FROM_ALL
  probes/src/entropy_probe.c)
target_include_directories(bk3633_entropy_probe PRIVATE
  "${BK3633_CONFIG_INCLUDE_DIR}" ${BK3633_SDK_INCLUDE_DIRS})

add_executable(bk3633_button_probe EXCLUDE_FROM_ALL
  probes/src/button_probe.c)
target_include_directories(bk3633_button_probe PRIVATE
  "${BK3633_CONFIG_INCLUDE_DIR}" ${BK3633_SDK_INCLUDE_DIRS})

add_executable(bk3633_buzzer_probe EXCLUDE_FROM_ALL
  probes/src/buzzer_probe.c)
target_include_directories(bk3633_buzzer_probe PRIVATE
  "${BK3633_CONFIG_INCLUDE_DIR}" ${BK3633_SDK_INCLUDE_DIRS})

add_executable(bk3633_system_event_probe EXCLUDE_FROM_ALL
  probe/system_event_probe.c
  src/h2_bk3633_platform_system_event.c
)
target_include_directories(bk3633_system_event_probe PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/include"
  "${FIRMWARES_ROOT}/libs/pal/include"
)

add_executable(bk3633_mem_probe EXCLUDE_FROM_ALL
  probe/mem_probe.c
  src/h2_bk3633_platform_mem.c
)
target_include_directories(bk3633_mem_probe PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/include"
  "${FIRMWARES_ROOT}/libs/pal/include"
)

add_executable(bk3633_log_probe EXCLUDE_FROM_ALL
  probe/log_probe.c
  src/h2_bk3633_platform_log.c
)
target_include_directories(bk3633_log_probe PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/include"
  "${FIRMWARES_ROOT}/libs/pal/include"
)

add_executable(bk3633_firmware_info_probe EXCLUDE_FROM_ALL
  probe/firmware_info_probe.c
  src/h2_bk3633_platform_firmware_info.c
)
target_include_directories(bk3633_firmware_info_probe PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/include"
  "${FIRMWARES_ROOT}/libs/pal/include"
)
target_compile_definitions(bk3633_firmware_info_probe PRIVATE
  H2_BK3633_FIRMWARE_VERSION="${H2_BK3633_EFFECTIVE_FIRMWARE_VERSION}")
