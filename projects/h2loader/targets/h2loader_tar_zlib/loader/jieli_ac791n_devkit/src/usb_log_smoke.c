#include "asm/includes.h"
#include "h2_jieli_ac791n_devkit.h"
#include "os/os_api.h"
#include "usb/device/cdc.h"
#include "usb/usb_common_def.h"

#include <stdint.h>

extern int printf(const char *format, ...);

static void usb_log_smoke_task(void *argument) {
  (void)argument;
  uint32_t heartbeat = 0u;
  for (;;) {
    printf("H2_JIELI_USB_LOG_SMOKE heartbeat=%u uptime_ms=%u\r\n",
           (unsigned)heartbeat++, (unsigned)timer_get_ms());
    os_time_dly(100u);
  }
}

void app_main(void) {
  int rc = h2_jieli_ac791n_devkit_usb_debug_start();
  printf("H2_JIELI_USB_LOG_SMOKE boot usb0_cdc_rc=%d\r\n", rc);
  if (rc != 0) return;

  rc = os_task_create(
      usb_log_smoke_task, NULL, 10, 1024, 0, "usb_log_smoke");
  printf("H2_JIELI_USB_LOG_SMOKE task_create_rc=%d\r\n", rc);
}
