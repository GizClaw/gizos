#ifndef H2_BK_TASK_CREATE_INTERNAL_H
#define H2_BK_TASK_CREATE_INTERNAL_H

#include "h2_bk_platform_core.h"
#include <os/os.h>
#include "FreeRTOS.h"

static inline int h2_bk_task_create(beken_thread_t* thread,
                                    const h2_bk_task_policy_t* policy,
                                    const char* name,
                                    beken_thread_function_t entry,
                                    uint32_t stack_size, void* ctx) {
#if defined(CONFIG_SOC_SMP) && CONFIG_SOC_SMP && defined(CONFIG_CPU_CNT) && \
    CONFIG_CPU_CNT > 1
  if (policy->core == H2_BK_TASK_CORE_ANY) {
    return policy->stack_region == H2_BK_TASK_STACK_PSRAM
               ? rtos_create_psram_thread(thread, (uint8_t)policy->priority,
                                          name, entry, stack_size, ctx)
               : rtos_create_thread(thread, (uint8_t)policy->priority, name,
                                    entry, stack_size, ctx);
  }
  if (policy->core == 1u) {
    return policy->stack_region == H2_BK_TASK_STACK_PSRAM
               ? rtos_core1_create_psram_thread(thread,
                                                (uint8_t)policy->priority, name,
                                                entry, stack_size, ctx)
               : rtos_core1_create_thread(thread, (uint8_t)policy->priority,
                                          name, entry, stack_size, ctx);
  }
  return policy->stack_region == H2_BK_TASK_STACK_PSRAM
             ? rtos_core0_create_psram_thread(thread, (uint8_t)policy->priority,
                                              name, entry, stack_size, ctx)
             : rtos_core0_create_thread(thread, (uint8_t)policy->priority, name,
                                        entry, stack_size, ctx);
#else
  if (policy->core != 0u && policy->core != H2_BK_TASK_CORE_ANY) {
    return kGeneralErr;
  }
  return policy->stack_region == H2_BK_TASK_STACK_PSRAM
             ? rtos_create_psram_thread(thread, (uint8_t)policy->priority, name,
                                        entry, stack_size, ctx)
             : rtos_create_thread(thread, (uint8_t)policy->priority, name,
                                  entry, stack_size, ctx);
#endif
}

#endif
