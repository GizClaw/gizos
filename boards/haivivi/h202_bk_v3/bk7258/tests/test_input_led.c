#include "h2_bk7258_board_private.h"
#include "h2_h202_board_test_sdk.h"

#include <assert.h>
#include <string.h>

static unsigned levels[56];
static uint32_t pin_state[56];
static int nfc_rc;
static int spi_rc;
static int restore_rc;
static unsigned config_calls;
static unsigned semaphore;
static spi_isr_t finish;
static uint8_t transmitted[256];
static unsigned tx_size;
static bool complete_tx = true;

int gpio_dev_unmap(gpio_id_t id) {
  (void)id;
  return 0;
}
int bk_gpio_set_config(gpio_id_t id, const gpio_config_t* config) {
  assert(id < 56 && config != NULL);
  ++config_calls;
  return 0;
}
int bk_gpio_set_output_low(gpio_id_t id) {
  levels[id] = 0;
  return 0;
}
int bk_gpio_set_output_high(gpio_id_t id) {
  levels[id] = 1;
  return 0;
}
int bk_gpio_get_input(gpio_id_t id) { return (int)levels[id]; }
uint32_t bk_gpio_get_value(gpio_id_t id) { return pin_state[id]; }
int bk_gpio_set_value(gpio_id_t id, uint32_t value) {
  pin_state[id] = value;
  return restore_rc;
}
int gpio_dev_map(gpio_id_t id, unsigned function) {
  assert((id == 38 && function == GPIO_DEV_I2C1_SCL) ||
         (id == 39 && function == GPIO_DEV_I2C1_SDA));
  return 0;
}
void rtos_delay_milliseconds(unsigned ms) { assert(ms == 10); }
h2_pal_result_t h2_bk_nfc_fm175xx_init(
    h2_bk_nfc_fm175xx_t* adapter, const h2_bk_nfc_fm175xx_config_t* config) {
  assert(adapter != NULL);
  assert(config->id == H2_H202_NFC_ID);
  assert(config->i2c_id == 1 && config->i2c_address == 0x28);
  assert(config->i2c_speed_hz == 100000 && config->read_capacity == 256);
  return nfc_rc;
}
const h2_pal_nfc_api_t* h2_bk_nfc_fm175xx_api(h2_bk_nfc_fm175xx_t* adapter) {
  return &adapter->api;
}
/* SARADC: the pin voltage, a read failure, and a balanced session. */
static uint16_t adc_pin_mv;
static int adc_read_rc;
static int adc_sessions;
int bk_adc_acquire(void) {
  ++adc_sessions;
  return 0;
}
int bk_adc_release(void) {
  --adc_sessions;
  return 0;
}
int bk_adc_init(adc_chan_t chan) {
  assert(chan == H2_H202_BATTERY_ADC_CHANNEL);
  return 0;
}
int bk_adc_deinit(adc_chan_t chan) {
  assert(chan == H2_H202_BATTERY_ADC_CHANNEL);
  return 0;
}
int bk_adc_set_config(adc_config_t* config) {
  assert(config->chan == H2_H202_BATTERY_ADC_CHANNEL);
  return 0;
}
int bk_adc_enable_bypass_clalibration(void) { return 0; }
int bk_adc_start(void) { return 0; }
int bk_adc_stop(void) { return 0; }
int bk_adc_read(uint16_t* data, uint32_t timeout) {
  assert(timeout == 100);
  *data = 1234;
  return adc_read_rc;
}
uint16_t bk_adc_data_calculate(uint16_t value, uint8_t chan) {
  assert(value == 1234 && chan == H2_H202_BATTERY_ADC_CHANNEL);
  return adc_pin_mv;
}
int rtos_init_semaphore(beken_semaphore_t* out, unsigned count) {
  assert(count == 1);
  *out = &semaphore;
  semaphore = 0;
  return 0;
}
int rtos_set_semaphore(beken_semaphore_t* value) {
  assert(*value == &semaphore);
  semaphore = 1;
  return 0;
}
int rtos_get_semaphore(beken_semaphore_t* value, unsigned timeout) {
  assert(*value == &semaphore && (timeout == 0 || timeout == 20));
  if (!semaphore) return -1;
  semaphore = 0;
  return 0;
}
int rtos_deinit_semaphore(beken_semaphore_t* value) {
  *value = NULL;
  return 0;
}
int bk_spi_driver_init(void) { return 0; }
int bk_spi_init(spi_id_t id, const spi_config_t* config) {
  assert(id == 1 && config->baud_rate == 3200000);
  assert(config->dma_mode == SPI_DMA_MODE_DISABLE);
  pin_state[2] = pin_state[3] = pin_state[5] = 0;
  return spi_rc;
}
int bk_spi_deinit(spi_id_t id) {
  assert(id == 1);
  return 0;
}
int bk_spi_register_tx_finish_isr(spi_id_t id, spi_isr_t cb, void* user) {
  assert(id == 1 && user == NULL);
  finish = cb;
  return 0;
}
int bk_spi_write_bytes_async(spi_id_t id, const void* data, unsigned size) {
  assert(id == 1 && size <= sizeof(transmitted));
  memcpy(transmitted, data, size);
  tx_size = size;
  if (complete_tx) finish(id, NULL);
  return 0;
}
/* PWM fake models ownership even when init/start partially fail. */
static bool pwm_live, pwm_running;
static int pwm_init_rc, pwm_start_rc, pwm_update_rc, pwm_stop_rc, pwm_deinit_rc;
static uint32_t pwm_cycles;
int bk_pwm_driver_init(void) { return 0; }
int bk_pwm_init(pwm_chan_t chan, const pwm_init_config_t* config) {
  assert(chan == 11u && !pwm_live);
  assert(config->period_cycle == 26000u && config->psc == 0u);
  assert(config->duty2_cycle == 0u && config->duty3_cycle == 0u);
  pwm_live = true;
  pwm_cycles = config->duty_cycle;
  return pwm_init_rc;
}
int bk_pwm_start(pwm_chan_t chan) {
  assert(chan == 11u && pwm_live);
  pwm_running = true;
  return pwm_start_rc;
}
int bk_pwm_set_period_duty(pwm_chan_t chan, pwm_period_duty_config_t* config) {
  assert(chan == 11u && pwm_live && pwm_running);
  assert(config->period_cycle == 26000u && config->psc == 0u);
  if (pwm_update_rc) return pwm_update_rc;
  pwm_cycles = config->duty_cycle;
  return 0;
}
int bk_pwm_stop(pwm_chan_t chan) {
  assert(chan == 11u && pwm_live && pwm_running);
  if (pwm_stop_rc) return pwm_stop_rc;
  pwm_running = false;
  return 0;
}
int bk_pwm_deinit(pwm_chan_t chan) {
  assert(chan == 11u && pwm_live && !pwm_running);
  if (pwm_deinit_rc) return pwm_deinit_rc;
  pwm_live = false;
  return 0;
}

static void test_inputs(void) {
  h2_pal_single_button_reading_t button;
  const h2_pal_button_api_t* api = h2_h202_button_api();
  assert(h2_pal_button_read_single_button(api, H2_H202_POWER_KEY_ID, &button) ==
         H2_PAL_ERR_INVALID_STATE);
  nfc_rc = H2_PAL_ERR_IO;
  assert(h2_h202_input_init() == H2_PAL_ERR_IO);
  /* NFC failure leaves physical navigation operational. */
  levels[H2_H202_POWER_KEY_GPIO] = 0;
  assert(h2_pal_button_read_single_button(api, H2_H202_POWER_KEY_ID, &button) ==
         0);
  assert(button.state == H2_PAL_BUTTON_STATE_PRESSED);
  assert(h2_h202_nfc_api() == NULL);
  nfc_rc = H2_PAL_OK;
  assert(h2_h202_input_init() == 0);
  unsigned calls = config_calls;
  assert(h2_h202_input_init() == 0 && config_calls == calls);
  const unsigned ids[] = {H2_H202_POWER_KEY_ID, H2_H202_RECORD_KEY_ID,
                          H2_H202_PLUS_KEY_ID, H2_H202_MINUS_KEY_ID};
  const unsigned pins[] = {2, 35, 33, 34};
  for (unsigned i = 0; i < 4; ++i) {
    levels[pins[i]] = 1;
    assert(h2_pal_button_read_single_button(api, ids[i], &button) == 0);
    assert(button.id == ids[i] && button.state == H2_PAL_BUTTON_STATE_RELEASED);
    levels[pins[i]] = 0;
    assert(h2_pal_button_read_single_button(api, ids[i], &button) == 0);
    assert(button.state == H2_PAL_BUTTON_STATE_PRESSED);
  }
  assert(h2_pal_button_read_single_button(api, 99, &button) ==
         H2_PAL_ERR_NOT_FOUND);
  /* USB_INSERT is active high; LGS4056 CHRG and DONE are active low. */
  const h2_pal_switch_api_t* lines = h2_h202_switch_api();
  const struct {
    unsigned id;
    unsigned pin;
    unsigned asserted;
  } charger[] = {{H2_H202_USB_INSERT_ID, 3, 1},
                 {H2_H202_CHARGING_ID, 30, 0},
                 {H2_H202_CHARGE_DONE_ID, 31, 0}};
  h2_pal_switch_state_t state;
  for (unsigned i = 0; i < 3; ++i) {
    levels[charger[i].pin] = charger[i].asserted;
    assert(h2_pal_switch_get(lines, charger[i].id, &state) == 0);
    assert(state == H2_PAL_SWITCH_STATE_ON);
    levels[charger[i].pin] = !charger[i].asserted;
    assert(h2_pal_switch_get(lines, charger[i].id, &state) == 0);
    assert(state == H2_PAL_SWITCH_STATE_OFF);
    assert(h2_pal_switch_set(lines, charger[i].id, H2_PAL_SWITCH_STATE_ON) ==
           H2_PAL_ERR_UNSUPPORTED);
  }
  assert(h2_pal_switch_get(lines, 99, &state) == H2_PAL_ERR_NOT_FOUND);
  /* Battery: VBAT through the 1/2 divider; no reading means absent. */
  const h2_pal_input_api_t* input = h2_h202_input_api();
  h2_pal_battery_reading_t battery;
  adc_pin_mv = 1900;
  assert(h2_pal_input_read_battery(input, H2_H202_BATTERY_ID, &battery) == 0);
  assert(battery.voltage_mv == 3800 &&
         (battery.flags & H2_PAL_BATTERY_PRESENT) != 0u &&
         (battery.flags & H2_PAL_BATTERY_HAS_VOLTAGE_MV) != 0u);
  adc_pin_mv = 12;
  assert(h2_pal_input_read_battery(input, H2_H202_BATTERY_ID, &battery) == 0);
  assert(battery.voltage_mv == 24 &&
         (battery.flags & H2_PAL_BATTERY_PRESENT) == 0u);
  adc_read_rc = -1;
  assert(h2_pal_input_read_battery(input, H2_H202_BATTERY_ID, &battery) ==
         H2_PAL_ERR_IO);
  adc_read_rc = 0;
  assert(adc_sessions == 0);
  assert(h2_pal_input_read_battery(input, 99, &battery) ==
         H2_PAL_ERR_NOT_FOUND);
}

static void test_lights(void) {
  const h2_pal_led_api_t* api = h2_h202_led_api();
  pin_state[2] = 0x122;
  pin_state[3] = 0x133;
  pin_state[5] = 0x155;
  spi_rc = -1;
  assert(h2_pal_led_clear(api, H2_H202_EYES_ID) == H2_PAL_ERR_IO);
  assert(pin_state[2] == 0x122 && pin_state[3] == 0x133 &&
         pin_state[5] == 0x155);
  spi_rc = 0;
  restore_rc = -1;
  assert(h2_pal_led_clear(api, H2_H202_EYES_ID) == H2_PAL_ERR_IO);
  restore_rc = 0;
  assert(h2_pal_led_set_brightness_percent(api, H2_H202_EYES_ID, 100) == 0);
  h2_pal_led_color_t pixels[H2_H202_RGB_PIXELS] = {0};
  pixels[0].g = 0xff;
  pixels[1].r = 0xff;
  pixels[5].b = 0xff;
  assert(h2_pal_led_set_frame(api, H2_H202_EYES_ID, pixels,
                              H2_H202_RGB_PIXELS) == 0);
  assert(tx_size == 200);
  for (unsigned i = 0; i < 64; ++i) {
    assert(transmitted[i] == 0 && transmitted[tx_size - 1 - i] == 0);
  }
  for (unsigned i = 0; i < 4; ++i) {
    assert(transmitted[64 + i] == 0xee);
    assert(transmitted[64 + 4 + i] == 0x88);
    assert(transmitted[64 + 12 + 4 + i] == 0xee);
    assert(transmitted[64 + 5 * 12 + 8 + i] == 0xee);
  }
  assert(h2_pal_led_set_frame(api, H2_H202_EYES_ID, pixels, 5) ==
         H2_PAL_ERR_INVALID_ARG);
  complete_tx = false;
  assert(h2_pal_led_clear(api, H2_H202_EYES_ID) == H2_PAL_ERR_IO);
  complete_tx = true;
  assert(h2_pal_led_clear(api, H2_H202_EYES_ID) == 0);
  const h2_pal_switch_api_t* lines = h2_h202_switch_api();
  h2_pal_switch_state_t white;
  assert(h2_pal_switch_set(lines, H2_H202_WHITE_ID, H2_PAL_SWITCH_STATE_ON) ==
         0);
  assert(levels[H2_H202_WHITE_LED_GPIO] == 1);
  assert(h2_pal_switch_get(lines, H2_H202_WHITE_ID, &white) == 0 &&
         white == H2_PAL_SWITCH_STATE_ON);
  const h2_pal_pwm_switch_api_t* pwm = h2_h202_pwm_switch_api();
  uint16_t duty;
  assert(h2_pal_pwm_switch_set_duty(pwm, 99u, 5000u) == H2_PAL_ERR_NOT_FOUND);
  assert(h2_pal_pwm_switch_set_duty(pwm, H2_H202_WHITE_ID, 10001u) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_pal_pwm_switch_get_duty(pwm, H2_H202_WHITE_ID, NULL) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_pal_pwm_switch_get_duty(pwm, 99u, &duty) == H2_PAL_ERR_NOT_FOUND);
  assert(h2_pal_pwm_switch_set_duty(pwm, H2_H202_WHITE_ID, 2500u) == 0);
  assert(pwm_live && pwm_running && pwm_cycles == 6500u);
  assert(h2_pal_pwm_switch_get_duty(pwm, H2_H202_WHITE_ID, &duty) == 0 &&
         duty == 2500u && h2_h202_white_on());
  assert(h2_pal_pwm_switch_set_duty(pwm, H2_H202_WHITE_ID, 7500u) == 0 &&
         pwm_cycles == 19500u);
  /* A failed stop/deinit remains owned until the next retry succeeds. */
  pwm_stop_rc = -1;
  assert(h2_pal_pwm_switch_set_duty(pwm, H2_H202_WHITE_ID, 0u) ==
         H2_PAL_ERR_IO);
  assert(pwm_live && pwm_running);
  pwm_stop_rc = 0;
  pwm_deinit_rc = -1;
  assert(h2_h202_led_deinit() == H2_PAL_ERR_IO && pwm_live && !pwm_running);
  pwm_deinit_rc = 0;
  assert(h2_pal_pwm_switch_set_duty(pwm, H2_H202_WHITE_ID, 10000u) == 0);
  assert(!pwm_live && levels[H2_H202_WHITE_LED_GPIO] == 1);
  pwm_init_rc = -1;
  assert(h2_pal_pwm_switch_set_duty(pwm, H2_H202_WHITE_ID, 5000u) ==
             H2_PAL_ERR_IO &&
         pwm_live && !pwm_running);
  pwm_init_rc = 0;
  pwm_start_rc = -1;
  assert(h2_pal_pwm_switch_set_duty(pwm, H2_H202_WHITE_ID, 5000u) ==
             H2_PAL_ERR_IO &&
         pwm_live && pwm_running);
  pwm_start_rc = 0;
  assert(h2_pal_pwm_switch_set_duty(pwm, H2_H202_WHITE_ID, 5000u) == 0);
  pwm_update_rc = -1;
  assert(h2_pal_pwm_switch_set_duty(pwm, H2_H202_WHITE_ID, 6000u) ==
         H2_PAL_ERR_IO);
  pwm_update_rc = 0;
  assert(h2_pal_pwm_switch_set_duty(pwm, H2_H202_WHITE_ID, 6000u) == 0 &&
         pwm_cycles == 15600u);
  /* The legacy switch can also reclaim a breathing LED. */
  assert(h2_pal_switch_set(lines, H2_H202_WHITE_ID, H2_PAL_SWITCH_STATE_OFF) ==
         0);
  assert(!pwm_live && !pwm_running && levels[H2_H202_WHITE_LED_GPIO] == 0);
  assert(h2_pal_pwm_switch_set_duty(pwm, H2_H202_WHITE_ID, 5000u) == 0);
  /* Deinit leaves the charge indicator dark, including an active PWM. */
  assert(h2_h202_led_deinit() == 0 && h2_h202_led_deinit() == 0);
  assert(levels[H2_H202_WHITE_LED_GPIO] == 0);
  assert(h2_pal_switch_get(lines, H2_H202_WHITE_ID, &white) == 0 &&
         white == H2_PAL_SWITCH_STATE_OFF);
}

int main(void) {
  test_inputs();
  test_lights();
  return 0;
}
