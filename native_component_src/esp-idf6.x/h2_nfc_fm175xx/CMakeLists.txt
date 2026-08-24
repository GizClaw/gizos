set(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")

include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_COMPONENT_SOURCES h2_nfc_fm175xx)

idf_component_register(
  SRCS ${H2_COMPONENT_SOURCES}
  INCLUDE_DIRS
    "include"
    "${CMAKE_CURRENT_LIST_DIR}/../../../libs/drivers/nfc/fm175xx/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../../libs/pal/include"
  PRIV_INCLUDE_DIRS
    "src"
  REQUIRES
    h2_firmware_lib
    esp_driver_gpio
    esp_driver_i2c
    esp_driver_spi
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)

if(CMAKE_BUILD_EARLY_EXPANSION)
  return()
endif()
