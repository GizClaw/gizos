#include "app_config.h"
#include "syscfg_id.h"
#include "typedef.h"

// Firmware composition policies used by the BR35 startup libraries.
const int config_asser = 0;

const int CONFIG_CPU_UNMASK_IRQ_ENABLE = 0;
const int MALLOC_MEMORY_DEFRAG_ENABLE = 0;
const int clock_sys_src_use_lrc_hw = 0;
const int config_debug_exception_record = 0;
const int config_mutex_irq_asser = 0;
const int const_malloc_list_check_add_extlen = 0;
const int const_malloc_list_check_debug_en = 0;
const int const_malloc_list_check_for_each = 0;
const int const_psram_malloc_list_check_add_extlen = 0;
const int const_psram_malloc_list_check_debug_en = 0;
const int const_psram_malloc_list_check_for_each = 0;
const u32 CONFIG_HEAP_MEMORY_TRACE = 0;
const u32 CONFIG_HEAP_MEMORY_MAX_TRACE_EN = 0;
const u32 lib_config_enable_auth_check = 0;

// No UART is selected on the generic chip board.
const char log_tag_const_d_CLOCK AT(.LOG_TAG_CONST) = 0;
const char log_tag_const_d_PSRAM_HEAP AT(.LOG_TAG_CONST) = 0;
const char log_tag_const_d_P_MEM_C AT(.LOG_TAG_CONST) = 0;
const char log_tag_const_d_RTC AT(.LOG_TAG_CONST) = 0;
const char log_tag_const_d_SYS_TMR AT(.LOG_TAG_CONST) = 0;
const char log_tag_const_e_CLOCK AT(.LOG_TAG_CONST) = 0;
const char log_tag_const_e_HEAP_MEM AT(.LOG_TAG_CONST) = 0;
const char log_tag_const_e_P_MEM_C AT(.LOG_TAG_CONST) = 0;
const char log_tag_const_e_TZFLASH AT(.LOG_TAG_CONST) = 0;
const char log_tag_const_i_HEAP_MEM AT(.LOG_TAG_CONST) = 0;
const char log_tag_const_i_MEM AT(.LOG_TAG_CONST) = 0;
const char log_tag_const_i_P_MEM_C AT(.LOG_TAG_CONST) = 0;
const char log_tag_const_i_SYS_TMR AT(.LOG_TAG_CONST) = 0;
const char log_tag_const_i_TZFLASH AT(.LOG_TAG_CONST) = 0;

const struct btif_item btif_table[] = {{0, 0}};
const char vm_ram_storage_enable = 0;
const int vm_max_page_align_size_config = TCFG_VM_SIZE;
const int vm_max_sector_align_size_config = TCFG_VM_SIZE;
const char log_tag_const_i_VM AT(.LOG_TAG_CONST) = 0;

// BR35 low-power diagnostic policies; the generic fixture has no debug UART.
const u32 sfc0_dtr_clk_freq = 128000000;
const u32 lib_xosc_auto_ctrl_enable = 1;
const u32 pdebug_uart_lowpower = 0;
const bool pdebug_putbyte_pdown = 0;
const bool pdebug_lp_dump_ram = 0;
const bool pdebug_xosc_resume = 0;
const bool pdebug_pdown_info = 0;
const char log_tag_const_i_CLOCK AT(.LOG_TAG_CONST) = 0;
const char log_tag_const_i_LP_TIMER AT(.LOG_TAG_CONST) = 0;
const char log_tag_const_i_PMU AT(.LOG_TAG_CONST) = 0;
// Same periodic idle wake policy as the vendor App.
int eSystemConfirmStopStatus(void) { return 0; }
