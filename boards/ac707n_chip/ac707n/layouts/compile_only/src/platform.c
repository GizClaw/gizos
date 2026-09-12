#include "app_config.h"
#include "asm/includes.h"
#include "system/includes.h"
#include "update.h"

extern void app_main(void);
extern void tick_timer_init(void);
extern void sys_timer_init(void);
const struct task_info task_info_table[] = {
    {.name = "app_core",
     .prio = 1,
     .core = 0,
     .stack_size = 4096,
     .qsize = 1024},
    {.name = "systimer", .prio = 6, .core = 0, .stack_size = 512, .qsize = 128},
    {0, 0}};
static void app_task(void *arg) {
  (void)arg;
  app_main();
  for (;;)
    os_time_dly(100);
}

// The SDK startup performs data relocation and BSS initialization before this
// hook. Keep all cache ways enabled; start the OS before the PAL E2E task.
void cache_ram_init(void) {}
u16 update_result_get(void) { return UPDATA_NON; }

void setup_arch(void) {
  wdt_close();
  memory_init();
  clk_early_init(PLL_REF_XOSC_DIFF, TCFG_CLOCK_OSC_HZ, 576 * MHz);
  os_init();
  tick_timer_init();
  sys_timer_init();
  if (os_task_create(app_task, NULL, 1, 4096, 1024, "app_core") == OS_NO_ERR)
    os_start();
  for (;;) {
    __asm__ volatile("idle");
  }
}

void cpu_assert_debug(void) {
  for (;;) {
    __asm__ volatile("idle");
  }
}
