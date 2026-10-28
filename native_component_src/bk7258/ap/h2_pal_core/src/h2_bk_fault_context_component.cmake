# The SDK owns the native exception entries. This component only retains
# the optional GizOS observers and its existing managed-log flush wrapper.
if(CONFIG_H2_BK_PLATFORM_FAULT_TRACE AND CONFIG_SOC_SMP AND CONFIG_SHELL_ASYNCLOG)
  armino_component_get_property(H2_BK_SHELL_LIB bk_cli COMPONENT_LIB)
  set_property(SOURCE "${BK_ARMINO_PATH}/components/bk_cli/shell_task.c"
    TARGET_DIRECTORY ${H2_BK_SHELL_LIB} APPEND PROPERTY COMPILE_OPTIONS "-fno-lto")
  target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,-u,bk_fault_context_enter"
    "-Wl,-u,bk_fault_context_observe"
    "-Wl,--wrap=shell_log_flush")
  if(CONFIG_H2_BK_PLATFORM_COREDUMP)
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,-u,bk_fault_context_persist")
  endif()
endif()
