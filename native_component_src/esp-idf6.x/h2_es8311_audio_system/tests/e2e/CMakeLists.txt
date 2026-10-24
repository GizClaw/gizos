include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_SOURCES h2_es8311_aec_e2e)
idf_component_register(SRCS ${H2_SOURCES} INCLUDE_DIRS "."
  REQUIRES h2_es8311_audio_system esp_timer esp_hw_support)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
