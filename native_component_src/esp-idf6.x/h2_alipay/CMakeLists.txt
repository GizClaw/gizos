include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_ALIPAY_SOURCES h2_alipay)
idf_component_register(SRCS ${H2_ALIPAY_SOURCES} INCLUDE_DIRS "include"
  REQUIRES h2_firmware_lib esp_hw_support esp_netif lwip freertos
  # Vendor archives introduce HAL references after component archive scanning.
  WHOLE_ARCHIVE)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu11 -Wall -Wextra -Werror)
