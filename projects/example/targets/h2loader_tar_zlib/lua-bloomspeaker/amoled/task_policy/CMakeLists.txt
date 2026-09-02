include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_TARGET_TASK_POLICY_SOURCES h2_esp_target_task_policy)

idf_component_register(
    SRCS ${H2_TARGET_TASK_POLICY_SOURCES}
    INCLUDE_DIRS "."
    REQUIRES h2_firmware_lib h2_pal_core)
