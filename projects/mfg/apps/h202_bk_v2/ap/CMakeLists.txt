include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_COMPONENT_SOURCES ap ap)

set(REPO_ROOT "$ENV{H2_GIZOS_ROOT}")
if("${REPO_ROOT}" STREQUAL "")
  get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)
endif()

armino_component_register(
  SRCS ${H2_COMPONENT_SOURCES}
  INCLUDE_DIRS
    "."
    "${REPO_ROOT}/projects/mfg/apps/h202_bk_v2/app/include"
    "${REPO_ROOT}/projects/h2loader/libs/h2loader/include"
    "${REPO_ROOT}/projects/h2loader/native_component_src/bk7258/ap/h2loader_bk/include"
    "${REPO_ROOT}/boards/h202_bk_v2/bk7258/include"
    "${REPO_ROOT}/boards/h202_bk_v2/bk7258/ap/include"
    "${REPO_ROOT}/native_component_src/bk7258/ap/h2_pal_core/include"
    "${REPO_ROOT}/libs/atomic/include"
    "${REPO_ROOT}/libs/bundle/include"
    "${REPO_ROOT}/libs/command/include"
    "${REPO_ROOT}/libs/pal/include"
    "${REPO_ROOT}/libs/runtime/include"
  REQUIRES
    h2_bk_target_task_policy
    h2_bk7258_board
    bk_wifi
    bk_cli
    driver
    h2_firmware_lib
    h2loader_bk
    h2_pal_core
    psa_mbedtls
    zlib)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=c11
    -Wall
    -Wextra
    -Werror
    -Wno-unused-parameter
    -Wno-type-limits)

# One IMA ADPCM C array per prompt declared in ../BUILD.bazel, encoded from
# raw 8 kHz mono PCM16LE by prompts/ima_adpcm.py. The generated source stays
# in the build tree and is never committed.
set(H2_H202_PROMPTS
    charge_insert charge_remove charge_passed nfc nfc_passed wifi_passed
    mic_left mic_right listen power plus minus aging waiting charge_failed
    nfc_failed wifi_failed mic_failed aging_failed incomplete unsaved
    poweroff_failed network boot retry retry_poweroff)
set(H2_H202_PROMPT_DIR "${REPO_ROOT}/projects/mfg/apps/h202_bk_v2/prompts")
set(H2_H202_PROMPT_ENCODER "${H2_H202_PROMPT_DIR}/ima_adpcm.py")
set(H2_H202_PROMPT_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/h2_h202_mfg_prompts.c")
set(H2_H202_PROMPT_ARGS)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${H2_H202_PROMPT_ENCODER}")
foreach(H2_PROMPT IN LISTS H2_H202_PROMPTS)
  set(H2_PROMPT_FILE "${H2_H202_PROMPT_DIR}/zh/${H2_PROMPT}.pcm")
  if(NOT EXISTS "${H2_PROMPT_FILE}")
    message(FATAL_ERROR "missing H202 MFG prompt: ${H2_PROMPT_FILE}")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${H2_PROMPT_FILE}")
  list(APPEND H2_H202_PROMPT_ARGS "${H2_PROMPT}=${H2_PROMPT_FILE}")
endforeach()
armino_build_get_property(H2_H202_PYTHON PYTHON)
execute_process(
  COMMAND "${H2_H202_PYTHON}" -I "${H2_H202_PROMPT_ENCODER}"
          "${H2_H202_PROMPT_SOURCE}" ${H2_H202_PROMPT_ARGS}
  RESULT_VARIABLE H2_H202_PROMPT_RESULT)
if(NOT H2_H202_PROMPT_RESULT EQUAL 0)
  message(FATAL_ERROR "H202 MFG prompt encoding failed: ${H2_H202_PROMPT_RESULT}")
endif()
target_sources(${COMPONENT_LIB} PRIVATE "${H2_H202_PROMPT_SOURCE}")
