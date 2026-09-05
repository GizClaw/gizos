#include "app_config.h"

#include "event/bt_event.h"
#include "event/device_event.h"
#include "event/key_event.h"
#include "os/os_api.h"
#include "system/includes.h"
#include "system/timer.h"
#include "update/dual_bank_updata_api.h"

const struct irq_info irq_info_table[] = {
#ifdef CONFIG_IPMASK_ENABLE
    {IRQ_SOFT5_IDX, 6, 0},
    {IRQ_SOFT4_IDX, 6, 1},
#endif
#if CPU_CORE_NUM == 1
    {IRQ_SOFT5_IDX, 7, 0},
    {IRQ_SOFT4_IDX, 7, 1},
    {-2, -2, -2},
#endif
    {-1, -1, -1},
};

const struct task_info task_info_table[] = {
    {"app_core", 15, 1024, 256},
    {"sys_event", 29, 512, 0},
    {"systimer", 14, 256, 0},
    {"sys_timer", 9, 512, 64},
#if CPU_CORE_NUM > 1
    {"#C0btctrler", 19, 512, 384},
    {"#C0btstack", 18, 1024, 384},
#else
    {"btctrler", 19, 512, 384},
    {"btstack", 18, 768, 384},
#endif
    {0, 0},
};

static int main_key_event_handler(struct key_event *key) {
  (void)key;
  return false;
}

static int main_dev_event_handler(struct device_event *event) {
  (void)event;
  return false;
}

void app_default_event_handler(struct sys_event *event) {
  switch (event->type) {
    case SYS_KEY_EVENT:
      (void)main_key_event_handler((struct key_event *)event->payload);
      break;
    case SYS_DEVICE_EVENT:
      (void)main_dev_event_handler((struct device_event *)event->payload);
      break;
    case SYS_BT_EVENT: {
      extern int ble_demo_bt_event_handler(struct sys_event *event);
      (void)ble_demo_bt_event_handler(event);
      break;
    }
    default:
      break;
  }
}

static void return_to_loader(void *user) {
  (void)user;
  (void)flash_update_clr_boot_info(CLEAR_APP_RUNNING_BANK);
  cpu_reset();
}

/* The canonical early-boot SDK patch calls this before normal initcalls.  A
 * two-minute window leaves enough time for one isolated central test while
 * still guaranteeing this diagnostic image cannot strand Loader. */
int h2_jieli_ac791n_devkit_early_app_boot(void) {
  uint16_t timer =
      sys_timeout_add_to_task("sys_timer", NULL, return_to_loader, 120000u);
  if (timer == 0u) {
    (void)flash_update_clr_boot_info(CLEAR_APP_RUNNING_BANK);
    cpu_reset();
    return -1;
  }
  return 0;
}

void app_main(void) {
  puts("H2_JIELI_VENDOR_BLE_SMOKE start window=120s baseline=official-demo "
       "uart=uart1 sdram=off dual-bank=on trace=disconnect-only pb1-wakeup=off csa2=off hard-reset=on retry-0x3e=on adv=legacy-20ms capture=v32-csa2-off\r\n");
  extern void bt_ble_module_init(void);
  puts("H2_JIELI_VENDOR_BLE_SMOKE enter=bt_ble_module_init\r\n");
  bt_ble_module_init();
  puts("H2_JIELI_VENDOR_BLE_SMOKE return=bt_ble_module_init\r\n");
}
