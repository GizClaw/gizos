#ifndef H2_BOARD_MODEM_RI_H
#define H2_BOARD_MODEM_RI_H
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_private/gpio.h"
#include <stdbool.h>
#include <stddef.h>

/* Final input restoration after Runtime/peripheral initialization. Reuse the
 * existing ISR service and handler: a standby mailbox read must not allocate
 * a service, create a transport, or change modem power. */
static inline esp_err_t h2_board_modem_ri_restore_input(bool installed) {
  esp_err_t rc = rtc_gpio_hold_dis(GPIO_NUM_0);
  if (rc == ESP_OK) rc = rtc_gpio_deinit(GPIO_NUM_0);
  if (rc == ESP_OK) rc = gpio_func_sel(GPIO_NUM_0, PIN_FUNC_GPIO);
  if (rc == ESP_OK) rc = gpio_set_direction(GPIO_NUM_0, GPIO_MODE_INPUT);
  if (rc == ESP_OK) rc = gpio_pullup_dis(GPIO_NUM_0);
  if (rc == ESP_OK) rc = gpio_pulldown_dis(GPIO_NUM_0);
  if (rc == ESP_OK) rc = gpio_set_intr_type(GPIO_NUM_0, GPIO_INTR_NEGEDGE);
  if (rc == ESP_OK && installed) rc = gpio_intr_enable(GPIO_NUM_0);
  return rc;
}

/* A pulse may already be active when input restoration completes. Consume
 * that real assertion once, while preserving a captured edge after it ends. */
static inline bool h2_board_modem_ri_level_take(bool pending, bool line_low,
                                                bool* was_low) {
  if (was_low == NULL) return pending;
  const bool asserted = line_low && !*was_low;
  *was_low = line_low;
  return pending || asserted;
}

/* GPIO0 RI belongs to this board. The global ISR service may have another
 * owner. */
static inline esp_err_t h2_board_modem_ri_start(bool* installed,
                                                void (*handler)(void*)) {
  /* A passive deep-sleep wake still owns RI through the RTC mux/hold. Restore
   * its input before arming the digital edge detector or reading its level. */
  esp_err_t rc = rtc_gpio_hold_dis(GPIO_NUM_0);
  if (rc == ESP_OK) rc = rtc_gpio_deinit(GPIO_NUM_0);
  if (rc != ESP_OK) return rc;
  const gpio_config_t config = {.pin_bit_mask = 1ULL << GPIO_NUM_0,
                                .mode = GPIO_MODE_INPUT,
                                .pull_up_en = GPIO_PULLUP_DISABLE,
                                .pull_down_en = GPIO_PULLDOWN_DISABLE,
                                .intr_type = GPIO_INTR_NEGEDGE};
  rc = gpio_config(&config);
  if (rc != ESP_OK) return rc;
  /* Allow GPIO and I2C to share a CPU vector instead of consuming a
   * dedicated one before the first hardware AES operation. */
  rc = gpio_install_isr_service(ESP_INTR_FLAG_SHARED);
  if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) return rc;
  if (*installed) {
    rc = gpio_isr_handler_remove(GPIO_NUM_0);
    if (rc != ESP_OK) return rc;
    *installed = false;
  }
  rc = gpio_isr_handler_add(GPIO_NUM_0, handler, NULL);
  if (rc == ESP_OK) *installed = true;
  return rc;
}
#endif
