#include "app_config.h"

#include "asm/includes.h"
#include "device/includes.h"
#include "os/os_api.h"
#include "system/includes.h"

const struct irq_info irq_info_table[] = {
#ifdef CONFIG_IPMASK_ENABLE
    {IRQ_SOFT5_IDX, 6, 0}, {IRQ_SOFT4_IDX, 6, 1},
#endif
#if CPU_CORE_NUM == 1
    {IRQ_SOFT5_IDX, 7, 0}, {IRQ_SOFT4_IDX, 7, 1}, {-2, -2, -2},
#endif
    {-1, -1, -1},
};

const struct task_info task_info_table[] = {
    {"app_core", 15, 2048, 1024},
    {"sys_event", 29, 512, 0},
    {"systimer", 14, 256, 0},
    {"sys_timer", 9, 512, 128},
    {0, 0, 0, 0, 0},
};

UART2_PLATFORM_DATA_BEGIN(uart2_data).baudrate = 1000000, .port = PORT_REMAP,
    .output_channel = OUTPUT_CHANNEL0, .tx_pin = IO_PORTB_03, .rx_pin = -1,
    .max_continue_recv_cnt = 1024, .idle_sys_clk_cnt = 500000,
    .clk_src = PLL_48M, .flags = UART_DEBUG, UART2_PLATFORM_DATA_END();

static const struct low_power_param power_param = {
    .config = TCFG_LOWPOWER_LOWPOWER_SEL,
    .btosc_disable = TCFG_LOWPOWER_BTOSC_DISABLE,
    .vddiom_lev = TCFG_LOWPOWER_VDDIOM_LEVEL,
    .vddiow_lev = TCFG_LOWPOWER_VDDIOW_LEVEL,
    .vdc14_dcdc = TRUE,
    .vdc14_lev = VDC14_VOL_SEL_LEVEL,
    .sysvdd_lev = SYSVDD_VOL_SEL_LEVEL,
    .vlvd_enable = TRUE,
    .vlvd_value = VLVD_SEL_25V,
};

REGISTER_DEVICES(device_table) = {
    {"uart2", &uart_dev_ops, (void *)&uart2_data},
    {"rtc", &rtc_dev_ops, NULL},
};

#ifdef CONFIG_DEBUG_ENABLE
void debug_uart_init(void) { uart_init(&uart2_data); }
#endif

void board_early_init(void) { devices_init(); }

void board_init(void) { power_init(&power_param); }
