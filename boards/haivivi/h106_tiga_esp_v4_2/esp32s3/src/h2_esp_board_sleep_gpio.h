#ifndef H2_ESP_BOARD_SLEEP_GPIO_H
#define H2_ESP_BOARD_SLEEP_GPIO_H

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"
#include "esp_private/gpio.h"
#include <stdbool.h>
#include <stdint.h>

/* gpio_config() emits SDK debug logs. These fixed board output operations
 * must also be safe inside the terminal admission's GPIO critical section. */
static inline esp_err_t h2_tiga_modem_output_config(gpio_num_t pin,
                                                    bool input_enabled) {
  esp_err_t err = rtc_gpio_deinit(pin);
  if (err == ESP_OK) err = gpio_func_sel(pin, PIN_FUNC_GPIO);
  if (err == ESP_OK)
    err = gpio_set_direction(
        pin, input_enabled ? GPIO_MODE_INPUT_OUTPUT : GPIO_MODE_OUTPUT);
  if (err == ESP_OK) err = gpio_pullup_dis(pin);
  if (err == ESP_OK) err = gpio_pulldown_dis(pin);
  if (err == ESP_OK) err = gpio_set_intr_type(pin, GPIO_INTR_DISABLE);
  if (err == ESP_OK) err = gpio_intr_disable(pin);
  return err;
}

/* Release retained inverted DTR and wake the modem during board startup. */
static inline esp_err_t h2_tiga_modem_dtr_wake_gpio(gpio_num_t pin) {
  esp_err_t err = h2_tiga_modem_output_config(pin, true);
  if (err == ESP_OK) err = gpio_set_level(pin, 1);
  if (err == ESP_OK) err = rtc_gpio_hold_dis(pin);
  return err;
}

/* Timer configuration for terminal commit: no log/worker/UART dependency. */
static inline esp_err_t h2_tiga_sleep_arm_timer(uint32_t delay_ms) {
  return delay_ms == 0u
             ? ESP_OK
             : esp_sleep_enable_timer_wakeup((uint64_t)delay_ms * 1000ULL);
}

static inline esp_err_t h2_tiga_sleep_rtc_hold_output(gpio_num_t gpio,
                                                      uint32_t level) {
  if (rtc_gpio_set_level(gpio, level) != ESP_OK ||
      rtc_gpio_init(gpio) != ESP_OK ||
      rtc_gpio_set_direction(gpio, RTC_GPIO_MODE_OUTPUT_ONLY) != ESP_OK ||
      rtc_gpio_pullup_dis(gpio) != ESP_OK ||
      rtc_gpio_pulldown_dis(gpio) != ESP_OK ||
      rtc_gpio_hold_dis(gpio) != ESP_OK || rtc_gpio_hold_en(gpio) != ESP_OK)
    return ESP_FAIL;
  return ESP_OK;
}

/* Both ordinary sleep and the supervisor use this exact rail/DTR policy. */
static inline esp_err_t h2_tiga_sleep_modem_hold(bool modem_on) {
  if (h2_tiga_sleep_rtc_hold_output(12, modem_on ? 1u : 0u) != ESP_OK)
    return ESP_FAIL;
  return h2_tiga_sleep_rtc_hold_output(6, 0u);
}

/* Only board-connected pads. Never touch MSPI/PSRAM GPIO26..37. */
#define H2_TIGA_SLEEP_ISOLATE_MASK                                       \
  ((1ULL << 1) | (1ULL << 2) | (1ULL << 3) | (1ULL << 4) | (1ULL << 9) | \
   (1ULL << 10) | (1ULL << 11) | (1ULL << 18) | (1ULL << 20))
#define H2_TIGA_SLEEP_LOW_MASK                                                \
  ((1ULL << 7) | (1ULL << 8) | (1ULL << 14) | (1ULL << 16) | (1ULL << 19) |   \
   (1ULL << 38) | (1ULL << 39) | (1ULL << 40) | (1ULL << 41) | (1ULL << 42) | \
   (1ULL << 45) | (1ULL << 46) | (1ULL << 47) | (1ULL << 48))
/* External flash stays powered: deselect it. Powered EC25 RX needs idle high.
 */
#define H2_TIGA_SLEEP_HIGH_MASK ((1ULL << 5) | (1ULL << 17))
#define H2_TIGA_SLEEP_CHARGE_MASK (1ULL << 13)
#define H2_TIGA_SLEEP_HOLD_MASK ((1ULL << 6) | (1ULL << 12) | (1ULL << 15))

static inline bool h2_tiga_sleep_modem_on(bool physical_off, bool powered,
                                          bool prepared) {
  return !physical_off && powered && prepared;
}

static inline uint64_t h2_tiga_sleep_wake_mask(bool physical_off) {
  return (1ULL << 21) | (physical_off ? 0u : 1ULL);
}

/* Restore ordinary pads after wake. Modem rail/DTR and UART TX retain their
 * own owner. Releasing TX before a UART exists exposes reset HP GPIO state
 * to the powered module instead of its retained idle-high level. */
static inline esp_err_t h2_tiga_sleep_gpio_restore(void) {
  for (int pin = 0; pin <= 48; ++pin) {
    const uint64_t bit = 1ULL << pin;
    if (((H2_TIGA_SLEEP_ISOLATE_MASK | H2_TIGA_SLEEP_CHARGE_MASK |
          h2_tiga_sleep_wake_mask(false)) &
         bit) != 0u) {
      if (rtc_gpio_hold_dis(pin) != ESP_OK || rtc_gpio_deinit(pin) != ESP_OK)
        return ESP_FAIL;
    }
    if (pin != 17 &&
        ((H2_TIGA_SLEEP_LOW_MASK | H2_TIGA_SLEEP_HIGH_MASK) & bit) != 0u &&
        gpio_hold_dis(pin) != ESP_OK)
      return ESP_FAIL;
  }
  gpio_deep_sleep_hold_dis();
  return ESP_OK;
}

/* Claim a retained UART TX only when its owner is ready. Preload the idle
 * value before releasing hold so resetting the HP GPIO registers cannot
 * produce a break on the powered module's RX input. */
static inline esp_err_t h2_tiga_modem_tx_claim_gpio(gpio_num_t pin, bool high) {
  esp_err_t rc = h2_tiga_modem_output_config(pin, false);
  if (rc == ESP_OK) rc = gpio_set_level(pin, high ? 1u : 0u);
  if (rc == ESP_OK) rc = rtc_gpio_hold_dis(pin);
  return rc;
}

static inline esp_err_t h2_tiga_sleep_hold_output(int pin, uint32_t level) {
  const gpio_config_t config = {
      .pin_bit_mask = 1ULL << pin,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  if (gpio_set_level(pin, level) != ESP_OK || gpio_config(&config) != ESP_OK ||
      gpio_set_level(pin, level) != ESP_OK || gpio_hold_dis(pin) != ESP_OK ||
      gpio_hold_en(pin) != ESP_OK)
    return ESP_FAIL;
  return ESP_OK;
}

/* No driver mutex, allocation, storage, UART drain or scheduler wait here. */
static inline esp_err_t h2_tiga_sleep_gpio_prepare(bool physical_off) {
  /* USB can keep the board powered after physical shutdown: apply the same
   * peripheral holds, but isolate RI and avoid backfeeding the disabled modem.
   */
  const uint64_t isolate_mask =
      H2_TIGA_SLEEP_ISOLATE_MASK | (physical_off ? 1ULL : 0u);
  const uint64_t high_mask =
      H2_TIGA_SLEEP_HIGH_MASK & ~(physical_off ? (1ULL << 17) : 0u);
  for (int pin = 0; pin <= 48; ++pin) {
    const uint64_t bit = 1ULL << pin;
    if ((isolate_mask & bit) != 0u) {
      if (rtc_gpio_isolate(pin) != ESP_OK) return ESP_FAIL;
    } else if (((H2_TIGA_SLEEP_LOW_MASK | H2_TIGA_SLEEP_HIGH_MASK) & bit) !=
               0u) {
      if (h2_tiga_sleep_hold_output(pin, (high_mask & bit) != 0u) != ESP_OK)
        return ESP_FAIL;
    }
  }
  const uint64_t wake_mask = h2_tiga_sleep_wake_mask(physical_off);
  const uint64_t input_mask = wake_mask | H2_TIGA_SLEEP_CHARGE_MASK;
  const gpio_config_t input = {
      .pin_bit_mask = input_mask,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  if (gpio_config(&input) != ESP_OK) return ESP_FAIL;
  for (int pin = 0; pin <= 21; ++pin) {
    if ((input_mask & (1ULL << pin)) == 0u) continue;
    /* isolate removes pulls, but ALSO disables input and holds that state.
     * Release that hold and restore RTC input before IDF prepares EXT1.
     * External board pull-ups provide the inactive high level. */
    if (rtc_gpio_isolate(pin) != ESP_OK || rtc_gpio_hold_dis(pin) != ESP_OK ||
        rtc_gpio_init(pin) != ESP_OK ||
        rtc_gpio_set_direction(pin, RTC_GPIO_MODE_INPUT_ONLY) != ESP_OK)
      return ESP_FAIL;
  }
  if (esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL) != ESP_OK)
    return ESP_FAIL;
  gpio_deep_sleep_hold_en();
  if (esp_sleep_enable_ext1_wakeup_io(wake_mask, ESP_EXT1_WAKEUP_ANY_LOW) !=
      ESP_OK)
    return ESP_FAIL;
  /* S3 GPIO13 is RTC IO. EXT0 high coexists with EXT1 low. */
  return esp_sleep_enable_ext0_wakeup(13, 1);
}

#endif
