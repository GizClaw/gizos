#include "h2_esp_board_sleep_gpio.h"
#include <assert.h>
#include <string.h>

static uint64_t isolated, held, inputs, outputs, highs, wake;
static uint64_t held_levels;
static int fail_pin = -1;
static unsigned config_calls, timer_calls;
static uint64_t timer_us;
static bool fail_timer;
static bool deep_hold, charge_wake, fail_charge;
esp_err_t gpio_config(const gpio_config_t* config) {
  ++config_calls;
  assert(config->pull_up_en == GPIO_PULLUP_DISABLE);
  assert(config->pull_down_en == GPIO_PULLDOWN_DISABLE);
  if (config->mode == GPIO_MODE_INPUT)
    inputs |= config->pin_bit_mask;
  else
    outputs |= config->pin_bit_mask;
  return ESP_OK;
}
esp_err_t gpio_set_direction(gpio_num_t pin, gpio_mode_t mode) {
  if (pin == fail_pin) return ESP_FAIL;
  if (mode == GPIO_MODE_INPUT_OUTPUT)
    inputs |= 1ULL << pin;
  else
    inputs &= ~(1ULL << pin);
  outputs |= 1ULL << pin;
  return ESP_OK;
}
esp_err_t gpio_func_sel(gpio_num_t pin, uint32_t function) {
  assert(function == PIN_FUNC_GPIO);
  return pin == fail_pin ? ESP_FAIL : ESP_OK;
}
esp_err_t gpio_pullup_dis(gpio_num_t pin) {
  (void)pin;
  return ESP_OK;
}
esp_err_t gpio_pulldown_dis(gpio_num_t pin) {
  (void)pin;
  return ESP_OK;
}
esp_err_t gpio_set_intr_type(gpio_num_t pin, gpio_int_type_t type) {
  (void)pin;
  assert(type == GPIO_INTR_DISABLE);
  return ESP_OK;
}
esp_err_t gpio_intr_disable(gpio_num_t pin) {
  (void)pin;
  return ESP_OK;
}
esp_err_t esp_sleep_enable_timer_wakeup(uint64_t time_us) {
  ++timer_calls;
  timer_us = time_us;
  return fail_timer ? ESP_FAIL : ESP_OK;
}
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level) {
  if (pin == fail_pin) return ESP_FAIL;
  if (level)
    highs |= 1ULL << pin;
  else
    highs &= ~(1ULL << pin);
  return ESP_OK;
}
esp_err_t gpio_hold_dis(gpio_num_t pin) {
  held &= ~(1ULL << pin);
  return ESP_OK;
}
esp_err_t gpio_hold_en(gpio_num_t pin) {
  held |= 1ULL << pin;
  held_levels = (held_levels & ~(1ULL << pin)) | (highs & (1ULL << pin));
  return ESP_OK;
}
void gpio_deep_sleep_hold_en(void) { deep_hold = true; }
void gpio_deep_sleep_hold_dis(void) { deep_hold = false; }
esp_err_t rtc_gpio_isolate(gpio_num_t pin) {
  assert(((1ULL << pin) & H2_TIGA_SLEEP_HOLD_MASK) == 0u);
  isolated |= 1ULL << pin;
  held |= 1ULL << pin;
  inputs &= ~(1ULL << pin);
  return pin == fail_pin ? ESP_FAIL : ESP_OK;
}
esp_err_t rtc_gpio_hold_dis(gpio_num_t pin) { return gpio_hold_dis(pin); }
esp_err_t rtc_gpio_init(gpio_num_t pin) {
  (void)pin;
  return ESP_OK;
}
esp_err_t rtc_gpio_set_direction(gpio_num_t pin, rtc_gpio_mode_t mode) {
  assert(mode == RTC_GPIO_MODE_INPUT_ONLY || mode == RTC_GPIO_MODE_OUTPUT_ONLY);
  assert((held & (1ULL << pin)) == 0u);
  if (mode == RTC_GPIO_MODE_INPUT_ONLY)
    inputs |= 1ULL << pin;
  else
    outputs |= 1ULL << pin;
  return ESP_OK;
}
esp_err_t rtc_gpio_deinit(gpio_num_t pin) {
  return pin == fail_pin ? ESP_FAIL : ESP_OK;
}
esp_err_t rtc_gpio_set_level(gpio_num_t pin, uint32_t level) {
  return gpio_set_level(pin, level);
}
esp_err_t rtc_gpio_pullup_dis(gpio_num_t pin) {
  (void)pin;
  return ESP_OK;
}
esp_err_t rtc_gpio_pulldown_dis(gpio_num_t pin) {
  (void)pin;
  return ESP_OK;
}
esp_err_t rtc_gpio_hold_en(gpio_num_t pin) { return gpio_hold_en(pin); }
esp_err_t esp_sleep_disable_wakeup_source(int source) {
  assert(source == ESP_SLEEP_WAKEUP_ALL);
  wake = 0u;
  return ESP_OK;
}
esp_err_t esp_sleep_enable_ext1_wakeup_io(uint64_t mask, int mode) {
  assert(mode == ESP_EXT1_WAKEUP_ANY_LOW);
  assert((inputs & mask) == mask);
  assert((held & mask) == 0u);
  wake = mask;
  return ESP_OK;
}
esp_err_t esp_sleep_enable_ext0_wakeup(int pin, int level) {
  assert(pin == 13 && level == 1);
  assert((inputs & (1ULL << pin)) != 0u);
  assert((held & (1ULL << pin)) == 0u);
  assert(wake != 0u); /* EXT0 must coexist with EXT1. */
  charge_wake = !fail_charge;
  return fail_charge ? ESP_FAIL : ESP_OK;
}
static void reset(void) {
  isolated = held = inputs = outputs = highs = wake = 0u;
  held_levels = 0u;
  deep_hold = charge_wake = fail_charge = fail_timer = false;
  fail_pin = -1;
  config_calls = timer_calls = 0u;
  timer_us = 0u;
}
int main(void) {
  /* A passive MCU wake has no UART to take over the retained TX output. */
  reset();
  assert(h2_tiga_sleep_gpio_prepare(false) == ESP_OK);
  assert(h2_tiga_sleep_modem_hold(true) == ESP_OK);
  highs = inputs = outputs =
      0u; /* HP GPIO registers reset; pad holds survive. */
  assert(((held & held_levels) & (1ULL << 17)) != 0u);
  assert(h2_tiga_sleep_gpio_restore() == ESP_OK);
  uint64_t levels = (held & held_levels) | (~held & highs);
  assert((levels & (1ULL << 17)) != 0u);
  assert((levels & (1ULL << 12)) != 0u);
  assert(h2_tiga_modem_tx_claim_gpio(17, true) == ESP_OK);
  assert((held & (1ULL << 17)) == 0u && (highs & (1ULL << 17)) != 0u);
  assert(gpio_set_level(17, 0u) == ESP_OK);
  levels = (held & held_levels) | (~held & highs);
  assert((levels & (1ULL << 17)) == 0u); /* UART can now drive data. */
  reset();
  assert(h2_tiga_sleep_arm_timer(0u) == ESP_OK && timer_calls == 0u);
  assert(h2_tiga_sleep_arm_timer(UINT32_MAX) == ESP_OK);
  assert(timer_us == (uint64_t)UINT32_MAX * 1000ULL);
  fail_timer = true;
  assert(h2_tiga_sleep_arm_timer(1u) == ESP_FAIL);
  reset();
  assert(h2_tiga_modem_output_config(12, false) == ESP_OK);
  assert(config_calls == 0u && (outputs & (1ULL << 12)) != 0u);
  assert(h2_tiga_modem_dtr_wake_gpio(6) == ESP_OK);
  assert(config_calls == 0u && (inputs & (1ULL << 6)) != 0u);
  /* Low-power output failures remain errors; no reset/panic helper is used. */
  fail_pin = 12;
  assert(h2_tiga_modem_output_config(12, false) == ESP_FAIL);

  for (unsigned physical = 0; physical < 2; ++physical) {
    for (unsigned powered = 0; powered < 2; ++powered) {
      for (unsigned prepared = 0; prepared < 2; ++prepared) {
        reset();
        const bool retained =
            h2_tiga_sleep_modem_on(physical, powered, prepared);
        assert(retained == (!physical && powered && prepared));
        assert(h2_tiga_sleep_gpio_prepare(!retained) == ESP_OK);
        assert(wake == ((1ULL << 21) | (retained ? 1u : 0u)));
        assert(h2_tiga_sleep_modem_hold(retained) == ESP_OK);
        assert((highs & (1ULL << 6)) == 0u && (held & (1ULL << 6)) != 0u);
        assert((highs & (1ULL << 12)) == (retained ? (1ULL << 12) : 0u));
        assert((held & (1ULL << 12)) != 0u);
        if (!retained) {
          assert((isolated & 1u) != 0u);
          assert((highs & (1ULL << 17)) == 0u);
        }
      }
    }
  }
  reset();
  fail_pin = 12;
  assert(h2_tiga_sleep_modem_hold(false) == ESP_FAIL);
  reset();
  fail_pin = 6;
  assert(h2_tiga_sleep_modem_hold(false) == ESP_FAIL);
  /* GPIO6 low is inverted DTR high in both terminal modes. */
  for (int physical_off = 0; physical_off <= 1; ++physical_off) {
    reset();
    assert(h2_tiga_modem_dtr_wake_gpio(6) == ESP_OK);
    assert((highs & (1ULL << 6)) != 0u);
    assert(h2_tiga_sleep_gpio_prepare(physical_off != 0) == ESP_OK);
    assert(charge_wake);
    assert(wake == (physical_off ? (1ULL << 21) : 0x200001u));
    assert((inputs & (1ULL << 13)) != 0u);
    assert((held & (1ULL << 13)) == 0u);
    assert(h2_tiga_sleep_rtc_hold_output(6, 0u) == ESP_OK);
    assert((highs & (1ULL << 6)) == 0u);
    assert((held & (1ULL << 6)) != 0u);
    assert((isolated & (1ULL << 6)) == 0u);
    /* A wake boot releases the retained low and wakes EC25 again. */
    assert(h2_tiga_modem_dtr_wake_gpio(6) == ESP_OK);
    assert((highs & (1ULL << 6)) != 0u);
    assert((held & (1ULL << 6)) == 0u);
  }
  reset();
  assert(h2_tiga_sleep_gpio_prepare(true) == ESP_OK);
  assert(wake == (1ULL << 21));
  assert((isolated & 1ULL) != 0u);      /* RI remains GPIO0. */
  assert((highs & (1ULL << 5)) != 0u);  /* External flash CS. */
  assert((highs & (1ULL << 17)) == 0u); /* No modem backfeed at power-off. */
  const uint64_t mspi_mask = ((1ULL << 38) - 1u) & ~((1ULL << 26) - 1u);
  assert(((isolated | held | inputs | outputs) & mspi_mask) == 0u);
  reset();
  fail_pin = 6;
  assert(h2_tiga_modem_dtr_wake_gpio(6) != ESP_OK);
  assert(h2_tiga_sleep_rtc_hold_output(6, 0u) != ESP_OK);
  reset();
  assert(h2_tiga_sleep_gpio_prepare(false) == ESP_OK);
  assert(wake == 0x200001u);
  assert(isolated ==
         (H2_TIGA_SLEEP_ISOLATE_MASK | wake | H2_TIGA_SLEEP_CHARGE_MASK));
  assert(held == (H2_TIGA_SLEEP_ISOLATE_MASK | H2_TIGA_SLEEP_LOW_MASK |
                  H2_TIGA_SLEEP_HIGH_MASK));
  assert(highs == H2_TIGA_SLEEP_HIGH_MASK && deep_hold);
  assert((outputs & H2_TIGA_SLEEP_HOLD_MASK) == 0u);
  reset();
  fail_pin = 2;
  assert(h2_tiga_sleep_gpio_prepare(false) != ESP_OK && wake == 0u);
  reset();
  fail_pin = 46;
  assert(h2_tiga_sleep_gpio_prepare(false) != ESP_OK && wake == 0u);
  assert(h2_tiga_sleep_wake_mask(true) == (1ULL << 21));
  reset();
  fail_charge = true;
  assert(h2_tiga_sleep_gpio_prepare(false) != ESP_OK);
  assert(!charge_wake);
  reset();
  assert(h2_tiga_sleep_gpio_prepare(true) == ESP_OK);
  assert(charge_wake && wake == (1ULL << 21));
  assert((held & (1ULL << 13)) == 0u);
  return 0;
}
