#include "h2_bk7258_board_private.h"

#include <string.h>

#ifdef H2_H202_HOST_TEST
#include "h2_h202_board_test_sdk.h"
#else
#include "driver/adc.h"
#include "driver/gpio.h"
#include "gpio_driver.h"
#include "h2_bk_nfc_fm175xx.h"
#include "os/os.h"
#endif

static h2_bk_nfc_fm175xx_t s_nfc;
static bool s_initialized;
static bool s_inputs_ready;

static h2_pal_result_t read_button(void* user, h2_pal_periph_id_t id,
                                   h2_pal_single_button_reading_t* out) {
  (void)user;
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  unsigned gpio;
  switch (id) {
    case H2_H202_POWER_KEY_ID:
      gpio = H2_H202_POWER_KEY_GPIO;
      break;
    case H2_H202_RECORD_KEY_ID:
      gpio = H2_H202_RECORD_KEY_GPIO;
      break;
    case H2_H202_PLUS_KEY_ID:
      gpio = H2_H202_PLUS_KEY_GPIO;
      break;
    case H2_H202_MINUS_KEY_ID:
      gpio = H2_H202_MINUS_KEY_GPIO;
      break;
    default:
      return H2_PAL_ERR_NOT_FOUND;
  }
  if (!s_inputs_ready) return H2_PAL_ERR_INVALID_STATE;
  out->id = id;
  out->state = bk_gpio_get_input((gpio_id_t)gpio) ? H2_PAL_BUTTON_STATE_RELEASED
                                                  : H2_PAL_BUTTON_STATE_PRESSED;
  return H2_PAL_OK;
}

/* Raw charger lines only; callers decode charger states. USB_INSERT is the
 * active-high VBUS divider, while LGS4056 CHRG and DONE are active-low. */
static h2_pal_result_t switch_get(void* user, h2_pal_periph_id_t id,
                                  h2_pal_switch_state_t* out) {
  (void)user;
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out = H2_PAL_SWITCH_STATE_OFF;
  unsigned gpio;
  int active;
  switch (id) {
    case H2_H202_USB_INSERT_ID:
      gpio = H2_H202_USB_INSERT_GPIO;
      active = 1;
      break;
    case H2_H202_CHARGING_ID:
      gpio = H2_H202_CHARGING_GPIO;
      active = 0;
      break;
    case H2_H202_CHARGE_DONE_ID:
      gpio = H2_H202_CHARGE_DONE_GPIO;
      active = 0;
      break;
    case H2_H202_WHITE_ID:
      if (h2_h202_white_on()) *out = H2_PAL_SWITCH_STATE_ON;
      return H2_PAL_OK;
    default:
      return H2_PAL_ERR_NOT_FOUND;
  }
  if (!s_inputs_ready) return H2_PAL_ERR_INVALID_STATE;
  if ((bk_gpio_get_input((gpio_id_t)gpio) != 0) == (active != 0))
    *out = H2_PAL_SWITCH_STATE_ON;
  return H2_PAL_OK;
}

static h2_pal_result_t switch_set(void* user, h2_pal_periph_id_t id,
                                  h2_pal_switch_state_t state) {
  (void)user;
  if (id == H2_H202_WHITE_ID)
    return h2_h202_white_set(state == H2_PAL_SWITCH_STATE_ON);
  return id == H2_H202_USB_INSERT_ID || id == H2_H202_CHARGING_ID ||
                 id == H2_H202_CHARGE_DONE_ID
             ? H2_PAL_ERR_UNSUPPORTED
             : H2_PAL_ERR_NOT_FOUND;
}

h2_pal_result_t h2_h202_input_init(void) {
  if (s_initialized) return H2_PAL_OK;
  const unsigned inputs[] = {
      H2_H202_POWER_KEY_GPIO,   H2_H202_RECORD_KEY_GPIO, H2_H202_PLUS_KEY_GPIO,
      H2_H202_MINUS_KEY_GPIO,   H2_H202_USB_INSERT_GPIO, H2_H202_CHARGING_GPIO,
      H2_H202_CHARGE_DONE_GPIO,
  };
  gpio_config_t input = {.io_mode = GPIO_INPUT_ENABLE,
                         .pull_mode = GPIO_PULL_UP_EN,
                         .func_mode = GPIO_SECOND_FUNC_DISABLE};
  for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
    gpio_id_t gpio = (gpio_id_t)inputs[i];
    input.pull_mode = inputs[i] == H2_H202_USB_INSERT_GPIO ? GPIO_PULL_DISABLE
                                                           : GPIO_PULL_UP_EN;
    if (gpio_dev_unmap(gpio) != 0 || bk_gpio_set_config(gpio, &input) != 0)
      return H2_PAL_ERR_IO;
  }
  /* A broken NFC device must not disable navigation and shutdown. */
  s_inputs_ready = true;
  gpio_config_t output = {.io_mode = GPIO_OUTPUT_ENABLE,
                          .pull_mode = GPIO_PULL_DISABLE,
                          .func_mode = GPIO_SECOND_FUNC_DISABLE};
  gpio_id_t reset = (gpio_id_t)H2_H202_NFC_RESET_GPIO;
  if (gpio_dev_unmap(reset) != 0 || bk_gpio_set_config(reset, &output) != 0 ||
      bk_gpio_set_output_low(reset) != 0)
    return H2_PAL_ERR_IO;
  rtos_delay_milliseconds(10);
  if (bk_gpio_set_output_high(reset) != 0 ||
      gpio_dev_unmap((gpio_id_t)H2_H202_NFC_SCL_GPIO) != 0 ||
      gpio_dev_unmap((gpio_id_t)H2_H202_NFC_SDA_GPIO) != 0 ||
      gpio_dev_map((gpio_id_t)H2_H202_NFC_SCL_GPIO, GPIO_DEV_I2C1_SCL) != 0 ||
      gpio_dev_map((gpio_id_t)H2_H202_NFC_SDA_GPIO, GPIO_DEV_I2C1_SDA) != 0)
    return H2_PAL_ERR_IO;
  rtos_delay_milliseconds(10);
  const h2_bk_nfc_fm175xx_config_t config = {
      .id = H2_H202_NFC_ID,
      .i2c_id = H2_H202_NFC_I2C_ID,
      .i2c_address = H2_H202_NFC_I2C_ADDRESS,
      .i2c_speed_hz = H2_H202_NFC_I2C_HZ,
      .read_capacity = H2_BK_NFC_FM175XX_DEFAULT_READ_CAPACITY,
  };
  h2_pal_result_t rc = h2_bk_nfc_fm175xx_init(&s_nfc, &config);
  if (rc == H2_PAL_OK) s_initialized = true;
  return rc;
}

/* The CP SARADC server drops the channel config on release, so every read
 * runs acquire, init, config, start, read, stop, deinit and release, as the
 * H106 Zero BK board does. */
static h2_pal_result_t battery_pin_mv(uint16_t* out) {
  static bool unmapped;
  const adc_chan_t chan = (adc_chan_t)H2_H202_BATTERY_ADC_CHANNEL;
  if (!unmapped) {
    (void)gpio_dev_unmap((gpio_id_t)H2_H202_BATTERY_ADC_GPIO);
    unmapped = true;
  }
  if (bk_adc_acquire() != 0) return H2_PAL_ERR_IO;
  h2_pal_result_t rc = bk_adc_init(chan) == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
  adc_config_t config;
  memset(&config, 0, sizeof(config));
  config.chan = chan;
  config.adc_mode = ADC_CONTINUOUS_MODE;
  config.src_clk = ADC_SCLK_XTAL_26M;
  config.clk = 3203125;
  config.saturate_mode = ADC_SATURATE_MODE_3;
  config.steady_ctrl = 7;
  if (rc == H2_PAL_OK &&
      (bk_adc_set_config(&config) != 0 ||
       bk_adc_enable_bypass_clalibration() != 0 || bk_adc_start() != 0))
    rc = H2_PAL_ERR_IO;
  if (rc == H2_PAL_OK) {
    uint16_t raw = 0;
    if (bk_adc_read(&raw, 100) != 0)
      rc = H2_PAL_ERR_IO;
    else
      *out = bk_adc_data_calculate(raw, (uint8_t)chan);
    (void)bk_adc_stop();
  }
  (void)bk_adc_deinit(chan);
  (void)bk_adc_release();
  return rc;
}

static h2_pal_result_t read_battery(void* user, h2_pal_periph_id_t id,
                                    h2_pal_battery_reading_t* out) {
  (void)user;
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (id != H2_H202_BATTERY_ID) return H2_PAL_ERR_NOT_FOUND;
  uint16_t pin_mv = 0;
  h2_pal_result_t rc = battery_pin_mv(&pin_mv);
  if (rc != H2_PAL_OK) return rc;
  const int32_t mv = (int32_t)pin_mv * (int32_t)H2_H202_BATTERY_DIVIDER;
  *out = (h2_pal_battery_reading_t){
      .id = id,
      .flags =
          H2_PAL_BATTERY_HAS_VOLTAGE_MV |
          (mv >= (int32_t)H2_H202_BATTERY_PRESENT_MV ? H2_PAL_BATTERY_PRESENT
                                                     : 0u),
      .voltage_mv = mv};
  return H2_PAL_OK;
}

const h2_pal_input_api_t* h2_h202_input_api(void) {
  static const h2_pal_input_vtable_t vtable = {.read_battery = read_battery};
  static const h2_pal_input_api_t api = {.vtable = &vtable};
  return &api;
}

const h2_pal_button_api_t* h2_h202_button_api(void) {
  static const h2_pal_button_vtable_t vtable = {.read_single_button =
                                                    read_button};
  static const h2_pal_button_api_t api = {.vtable = &vtable};
  return &api;
}

const h2_pal_switch_api_t* h2_h202_switch_api(void) {
  static const h2_pal_switch_vtable_t vtable = {.set = switch_set,
                                                .get = switch_get};
  static const h2_pal_switch_api_t api = {.vtable = &vtable};
  return &api;
}

const h2_pal_nfc_api_t* h2_h202_nfc_api(void) {
  return s_initialized ? h2_bk_nfc_fm175xx_api(&s_nfc) : NULL;
}
