set(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")

include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_COMPONENT_SOURCES h2_gizclaw)

idf_component_register(
  SRCS ${H2_COMPONENT_SOURCES}
  INCLUDE_DIRS
    "${REPO_ROOT}/libs/gizclaw/include"
  REQUIRES
    h2_firmware_lib
    esp_audio_codec
    esp_hw_support
    esp_timer
    h2_pal_core
  PRIV_REQUIRES
    opus_port)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
target_link_options(${COMPONENT_LIB} INTERFACE
  "-Wl,--undefined=h2_gizclaw_conversation_invalidate_internal"
  "-Wl,--undefined=gzc_default_platform")

if(CMAKE_BUILD_EARLY_EXPANSION)
  return()
endif()
