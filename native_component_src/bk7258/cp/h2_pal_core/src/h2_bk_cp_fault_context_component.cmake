# The SDK owns CP fault/NMI entries; retain only the configured observers.
if(CONFIG_H2_BK_PLATFORM_CP_FAULT_TRACE AND CONFIG_ARCH_CM33 AND NOT CONFIG_SOC_SMP)
  target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,-u,bk_fault_context_enter"
    "-Wl,-u,bk_fault_context_observe")
  if(CONFIG_H2_BK_PLATFORM_CP_COREDUMP)
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,-u,bk_fault_context_persist")
  endif()
endif()
