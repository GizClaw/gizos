if(CONFIG_H2_BK_PLATFORM_FAULT_TRACE AND CONFIG_SOC_SMP AND CONFIG_SHELL_ASYNCLOG)
  armino_component_get_property(H2_BK_TRAP_LIB cm33 COMPONENT_LIB)
  set(H2_BK_FAULT_INPUT "${BK_ARMINO_PATH}/middleware/arch/cm33/trap_base_smp.c")
  set(H2_BK_FAULT_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/h2_bk_sdk_trap_base_smp.c")
  include("${CMAKE_CURRENT_LIST_DIR}/h2_bk_fault_context_overlay.cmake")
  get_target_property(H2_BK_TRAP_SOURCES ${H2_BK_TRAP_LIB} SOURCES)
  set(H2_BK_TRAP_ORIGINALS ${H2_BK_TRAP_SOURCES})
  list(FILTER H2_BK_TRAP_ORIGINALS INCLUDE REGEX "(^|/)trap_base_smp\\.c$")
  list(LENGTH H2_BK_TRAP_ORIGINALS H2_BK_TRAP_SOURCE_COUNT)
  if(NOT H2_BK_TRAP_SOURCE_COUNT EQUAL 1)
    message(FATAL_ERROR "Expected one SDK AP trap source")
  endif()
  list(FILTER H2_BK_TRAP_SOURCES EXCLUDE REGEX "(^|/)trap_base_smp\\.c$")
  list(APPEND H2_BK_TRAP_SOURCES "${H2_BK_FAULT_OUTPUT}")
  set_property(TARGET ${H2_BK_TRAP_LIB} PROPERTY SOURCES ${H2_BK_TRAP_SOURCES})

  # Flush callers outside bk_cli must remain external references so that the
  # linker wrapper also observes the SDK assertion's pre-dump flush.
  armino_component_get_property(H2_BK_SHELL_LIB bk_cli COMPONENT_LIB)
  set(H2_BK_SHELL_SOURCE "${BK_ARMINO_PATH}/components/bk_cli/shell_task.c")
  get_source_file_property(H2_BK_SHELL_OPTIONS "${H2_BK_SHELL_SOURCE}"
    TARGET_DIRECTORY ${H2_BK_SHELL_LIB} COMPILE_OPTIONS)
  if(NOT H2_BK_SHELL_OPTIONS)
    set(H2_BK_SHELL_OPTIONS "")
  endif()
  list(APPEND H2_BK_SHELL_OPTIONS "-fno-lto")
  set_source_files_properties("${H2_BK_SHELL_SOURCE}"
    TARGET_DIRECTORY ${H2_BK_SHELL_LIB}
    PROPERTIES COMPILE_OPTIONS "${H2_BK_SHELL_OPTIONS}")
  target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,-u,h2_bk_fault_context_observe"
    "-Wl,--wrap=shell_log_flush")
endif()
