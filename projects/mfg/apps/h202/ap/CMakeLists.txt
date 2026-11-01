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
    "${REPO_ROOT}/projects/mfg/apps/h202/app/include"
    "${REPO_ROOT}/projects/h2loader/libs/h2loader/include"
    "${REPO_ROOT}/projects/h2loader/native_component_src/bk7258/ap/h2loader_bk/include"
    "${REPO_ROOT}/boards/h202/bk7258/include"
    "${REPO_ROOT}/boards/h202/bk7258/ap/include"
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

# One C array per prompt declared in ../BUILD.bazel; raw 8 kHz mono PCM16LE.
# The generated source stays in the build tree and is never committed.
set(H2_H202_PROMPTS
    charge_insert charge_remove charge_passed nfc nfc_passed wifi_passed
    cellular_passed mic_left mic_right listen power plus minus aging waiting
    charge_failed nfc_failed wifi_failed cellular_failed mic_failed
    aging_failed incomplete unsaved poweroff_failed)
set(H2_H202_PROMPT_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/h2_h202_mfg_prompts.c")
set(H2_H202_PROMPT_TEXT "#include <stddef.h>\n")
foreach(H2_PROMPT IN LISTS H2_H202_PROMPTS)
  set(H2_PROMPT_FILE "${REPO_ROOT}/projects/mfg/apps/h202/prompts/zh/${H2_PROMPT}.pcm")
  if(NOT EXISTS "${H2_PROMPT_FILE}")
    message(FATAL_ERROR "missing H202 MFG prompt: ${H2_PROMPT_FILE}")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${H2_PROMPT_FILE}")
  file(READ "${H2_PROMPT_FILE}" H2_PCM_HEX HEX)
  string(LENGTH "${H2_PCM_HEX}" H2_PCM_DIGITS)
  math(EXPR H2_PCM_ODD "${H2_PCM_DIGITS} % 4")
  if(H2_PCM_DIGITS EQUAL 0 OR NOT H2_PCM_ODD EQUAL 0)
    message(FATAL_ERROR "H202 MFG prompt is not whole PCM16 samples: ${H2_PROMPT_FILE}")
  endif()
  string(REGEX REPLACE "(..)" "0x\\1," H2_PCM_BYTES "${H2_PCM_HEX}")
  string(APPEND H2_H202_PROMPT_TEXT
    "const unsigned char h2_h202_prompt_${H2_PROMPT}[] = {${H2_PCM_BYTES}};\n"
    "const size_t h2_h202_prompt_${H2_PROMPT}_size = sizeof(h2_h202_prompt_${H2_PROMPT});\n")
endforeach()
file(WRITE "${H2_H202_PROMPT_SOURCE}.in" "${H2_H202_PROMPT_TEXT}")
configure_file("${H2_H202_PROMPT_SOURCE}.in" "${H2_H202_PROMPT_SOURCE}" COPYONLY)
target_sources(${COMPONENT_LIB} PRIVATE "${H2_H202_PROMPT_SOURCE}")
