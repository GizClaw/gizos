set(REPO_ROOT "$ENV{H2_GIZOS_ROOT}")
include("${REPO_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_COMPONENT_SOURCES h2_pal_storage ap)
set(H2_STORAGE_REQUIRES bk_common bk_rtos driver)
if(CONFIG_FATFS_SDCARD)
  list(APPEND H2_STORAGE_REQUIRES fatfs)
endif()
armino_component_register(
  SRCS ${H2_COMPONENT_SOURCES}
  INCLUDE_DIRS "include" "${REPO_ROOT}/libs/pal/include"
  PRIV_REQUIRES ${H2_STORAGE_REQUIRES})
target_compile_definitions(${COMPONENT_LIB} PRIVATE
  H2_BK_SD_FS_ENABLED=$<BOOL:${CONFIG_FATFS_SDCARD}>)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter)
