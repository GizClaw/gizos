#include "app_config.h"

#include "asm/power/p33.h"
#include "asm/pwm_led.h"
#include "syscfg_id.h"
#include "update.h"

// The pinned BR23 libraries deliberately leave these policies to the firmware
// composition. A compile-only layout disables optional product behavior while
// retaining the SDK's core startup, scheduler, VM, and linker contracts.
const int config_printf_time = 0;
const int config_asser = 0;
const int config_system_info = 0;
const int SDFILE_VFS_REDUCE_ENABLE = 0;
const int clock_sys_src_use_lrc_hw = 0;
const int support_vm_data_keep = 0;
const int vm_max_size_config = VM_MAX_SIZE_CONFIG;

const struct btif_item btif_table[] = {
    {0, 0},
};

#define H2_DISABLED_LOG_TAG(level, tag)                                        \
  const char log_tag_const_##level##_##tag AT(.LOG_TAG_CONST) = 0

H2_DISABLED_LOG_TAG(i, TMR);
H2_DISABLED_LOG_TAG(i, CLOCK);
H2_DISABLED_LOG_TAG(d, CLOCK);
H2_DISABLED_LOG_TAG(i, VM);
H2_DISABLED_LOG_TAG(d, VM);
H2_DISABLED_LOG_TAG(e, VM);
H2_DISABLED_LOG_TAG(i, LP_TIMER);
H2_DISABLED_LOG_TAG(i, LRC);
H2_DISABLED_LOG_TAG(i, PMU);
H2_DISABLED_LOG_TAG(d, PMU);
H2_DISABLED_LOG_TAG(c, PMU);
H2_DISABLED_LOG_TAG(i, P_MEM);
H2_DISABLED_LOG_TAG(e, P_MEM);
H2_DISABLED_LOG_TAG(i, V_MEM);
H2_DISABLED_LOG_TAG(i, SYS_TMR);
H2_DISABLED_LOG_TAG(d, SYS_TMR);
H2_DISABLED_LOG_TAG(e, SYS_TMR);
H2_DISABLED_LOG_TAG(i, KTASK);
H2_DISABLED_LOG_TAG(e, KTASK);
H2_DISABLED_LOG_TAG(i, HEAP_MEM);
H2_DISABLED_LOG_TAG(d, HEAP_MEM);
H2_DISABLED_LOG_TAG(e, HEAP_MEM);

u16 update_result_get(void) { return UPDATA_NON; }

u8 is_pwm_led_on(void) { return 0; }

u8 chg_reg_get(u8 addr) {
  (void)addr;
  return 0;
}
