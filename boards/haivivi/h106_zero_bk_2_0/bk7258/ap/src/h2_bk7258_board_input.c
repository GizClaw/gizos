#include "h2_bk7258_board_private.h"
#include "h2_bk_nfc_fm175xx.h"
#include "h2_zero_bk_board_config.h"

#include <common/bk_err.h>
#include <driver/adc.h>
#include <driver/gpio.h>
#include <os/os.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gpio_driver.h"

extern bk_err_t bk_phy_get_current_temperature(float* temperature);

typedef struct h2_zero_bk_button {
  h2_pal_periph_id_t id;
  gpio_id_t gpio;
  const char* name;
} h2_zero_bk_button_t;

static const h2_zero_bk_button_t s_buttons[] = {
    {
        .id = H2_ZERO_BK_POWER_BUTTON_ID,
        .gpio = (gpio_id_t)H2_ZERO_BK_POWER_BUTTON_GPIO,
        .name = "power",
    },
    {
        .id = H2_ZERO_BK_BUTTON_OK_ID,
        .gpio = (gpio_id_t)H2_ZERO_BK_BUTTON_OK_GPIO,
        .name = "ok",
    },
    {
        .id = H2_ZERO_BK_BUTTON_LEFT_ID,
        .gpio = (gpio_id_t)H2_ZERO_BK_BUTTON_LEFT_GPIO,
        .name = "left",
    },
    {
        .id = H2_ZERO_BK_BUTTON_RIGHT_ID,
        .gpio = (gpio_id_t)H2_ZERO_BK_BUTTON_RIGHT_GPIO,
        .name = "right",
    },
    {
        .id = H2_ZERO_BK_BUTTON_CANCEL_ID,
        .gpio = (gpio_id_t)H2_ZERO_BK_BUTTON_CANCEL_GPIO,
        .name = "cancel",
    },
    {
        .id = H2_ZERO_BK_BUTTON_VOLUME_UP_ID,
        .gpio = (gpio_id_t)H2_ZERO_BK_BUTTON_VOLUME_UP_GPIO,
        .name = "volume_up",
    },
    {
        .id = H2_ZERO_BK_BUTTON_VOLUME_DOWN_ID,
        .gpio = (gpio_id_t)H2_ZERO_BK_BUTTON_VOLUME_DOWN_GPIO,
        .name = "volume_down",
    },
    {
        .id = H2_ZERO_BK_BUTTON_RECORD_ID,
        .gpio = (gpio_id_t)H2_ZERO_BK_BUTTON_RECORD_GPIO,
        .name = "record",
    },
};

static const h2_pal_periph_led_strip_payload_t s_led_strip_payload = {
    .data_gpio_pin_id = H2_ZERO_BK_LED_STRIP_GPIO,
    .led_count = H2_ZERO_BK_LED_STRIP_PIXEL_COUNT,
    .channels_per_led = 3u,
};

static const h2_pal_periph_pwm_switch_payload_t s_vibration_payload = {
    .pwm_channel_id = H2_ZERO_BK_VIBRATION_PWM_ID,
    .gpio_pin_id = H2_ZERO_BK_VIBRATION_GPIO,
    .frequency_hz = H2_ZERO_BK_VIBRATION_FREQUENCY_HZ,
    .duty_resolution_bits = H2_ZERO_BK_VIBRATION_DUTY_RESOLUTION_BITS,
};

static uint32_t s_button_configured_mask;
static bool s_battery_adc_initialized;
static bool s_usb_insert_configured;
static h2_bk_nfc_fm175xx_t s_nfc;
static int s_nfc_initialized;

static h2_pal_result_t map_bk_rc(bk_err_t rc) {
  if (rc == BK_OK) {
    return H2_PAL_OK;
  }
  if (rc == BK_ERR_NULL_PARAM || rc == BK_ERR_PARAM) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return H2_PAL_ERR_IO;
}

/* The CP SARADC server drops the channel config on release, so every read
 * follows the SDK bat_monitor order: acquire, init, config, start, read,
 * stop, deinit, release. */
static h2_pal_result_t battery_adc_configure(void) {
  if (!s_battery_adc_initialized) {
    (void)gpio_dev_unmap((gpio_id_t)H2_ZERO_BK_BATTERY_ADC_GPIO);
    s_battery_adc_initialized = true;
  }

  h2_pal_result_t rc =
      map_bk_rc(bk_adc_init((adc_chan_t)H2_ZERO_BK_BATTERY_ADC_CHANNEL));
  if (rc != H2_PAL_OK) {
    return rc;
  }

  adc_config_t config;
  memset(&config, 0, sizeof(config));
  config.chan = (adc_chan_t)H2_ZERO_BK_BATTERY_ADC_CHANNEL;
  config.adc_mode = ADC_CONTINUOUS_MODE;
  config.src_clk = ADC_SCLK_XTAL_26M;
  config.clk = 3203125;
  config.saturate_mode = ADC_SATURATE_MODE_3;
  config.steady_ctrl = 7;
  config.adc_filter = 0;

  rc = map_bk_rc(bk_adc_set_config(&config));
  if (rc != H2_PAL_OK) {
    return rc;
  }
  return map_bk_rc(bk_adc_enable_bypass_clalibration());
}

static h2_pal_result_t battery_adc_read_pin_mv(uint16_t* out_pin_mv) {
  if (out_pin_mv == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (bk_adc_acquire() != BK_OK) {
    return H2_PAL_ERR_IO;
  }

  h2_pal_result_t rc = battery_adc_configure();
  if (rc != H2_PAL_OK) {
    goto out_deinit;
  }
  if (bk_adc_start() != BK_OK) {
    rc = H2_PAL_ERR_IO;
    goto out_deinit;
  }
  uint16_t raw = 0u;
  if (bk_adc_read(&raw, 100u) != BK_OK) {
    rc = H2_PAL_ERR_IO;
  } else {
    *out_pin_mv =
        bk_adc_data_calculate(raw, (UINT8)H2_ZERO_BK_BATTERY_ADC_CHANNEL);
  }
  (void)bk_adc_stop();

out_deinit:
  (void)bk_adc_deinit((adc_chan_t)H2_ZERO_BK_BATTERY_ADC_CHANNEL);
  (void)bk_adc_release();
  return rc;
}

static h2_pal_result_t usb_insert_read(bool* out_inserted) {
  if (out_inserted == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  const gpio_id_t gpio = (gpio_id_t)H2_ZERO_BK_USB_INSERT_GPIO;
  if (!s_usb_insert_configured) {
    const gpio_config_t config = {
        .io_mode = GPIO_INPUT_ENABLE,
        .pull_mode = GPIO_PULL_DISABLE,
        .func_mode = GPIO_SECOND_FUNC_DISABLE,
    };
    (void)gpio_dev_unmap(gpio);
    if (bk_gpio_set_config(gpio, &config) != BK_OK) {
      return H2_PAL_ERR_IO;
    }
    s_usb_insert_configured = true;
  }
  *out_inserted =
      (uint32_t)bk_gpio_get_input(gpio) == H2_ZERO_BK_USB_INSERT_ACTIVE_LEVEL;
  return H2_PAL_OK;
}

static int32_t battery_voltage_mv_from_pin_mv(uint16_t pin_mv) {
  return (int32_t)(((uint32_t)pin_mv * H2_ZERO_BK_BATTERY_DIVIDER_NUMERATOR) /
                   H2_ZERO_BK_BATTERY_DIVIDER_DENOMINATOR);
}

static uint16_t battery_percent_x100(int32_t voltage_mv) {
  if (voltage_mv <= (int32_t)H2_ZERO_BK_BATTERY_EMPTY_MV) {
    return 0u;
  }
  if (voltage_mv >= (int32_t)H2_ZERO_BK_BATTERY_FULL_MV) {
    return 10000u;
  }
  return (
      uint16_t)(((uint32_t)(voltage_mv - (int32_t)H2_ZERO_BK_BATTERY_EMPTY_MV) *
                 10000u) /
                (H2_ZERO_BK_BATTERY_FULL_MV - H2_ZERO_BK_BATTERY_EMPTY_MV));
}

static h2_pal_result_t nfc_board_power_reset(void) {
  const gpio_config_t output = {
      .io_mode = GPIO_OUTPUT_ENABLE,
      .pull_mode = GPIO_PULL_DISABLE,
      .func_mode = GPIO_SECOND_FUNC_DISABLE,
  };
  gpio_id_t reset = (gpio_id_t)H2_ZERO_BK_NFC_RESET_GPIO;

  const h2_pal_result_t rc = h2_zero_bk_peripheral_power_enable();
  if (rc != H2_PAL_OK) return rc;
  if (gpio_dev_unmap(reset) != BK_OK ||
      bk_gpio_set_config(reset, &output) != BK_OK ||
      bk_gpio_set_output_low(reset) != BK_OK)
    return H2_PAL_ERR_IO;
  rtos_delay_milliseconds(10u);
  if (bk_gpio_set_output_high(reset) != BK_OK) return H2_PAL_ERR_IO;
  rtos_delay_milliseconds(10u);
  return H2_PAL_OK;
}

static const h2_zero_bk_button_t* find_button(h2_pal_periph_id_t id,
                                              size_t* out_index) {
  for (size_t i = 0u; i < sizeof(s_buttons) / sizeof(s_buttons[0]); ++i) {
    if (s_buttons[i].id == id) {
      if (out_index != NULL) {
        *out_index = i;
      }
      return &s_buttons[i];
    }
  }
  return NULL;
}

static int known_periph_id(h2_pal_periph_id_t id) {
  return find_button(id, NULL) != NULL || id == H2_ZERO_BK_NFC_READER_ID ||
         id == H2_ZERO_BK_BATTERY_ID || id == H2_ZERO_BK_LED_STRIP_ID ||
         id == H2_ZERO_BK_SOC_TEMPERATURE_ID || id == H2_ZERO_BK_VIBRATION_ID;
}

static h2_pal_result_t configure_button(const h2_zero_bk_button_t* button,
                                        size_t index) {
  if ((s_button_configured_mask & (1u << index)) != 0u) {
    return H2_PAL_OK;
  }
  const gpio_config_t config = {
      .io_mode = GPIO_INPUT_ENABLE,
      .pull_mode = GPIO_PULL_UP_EN,
      .func_mode = GPIO_SECOND_FUNC_DISABLE,
  };
  (void)gpio_dev_unmap(button->gpio);
  if (bk_gpio_set_config(button->gpio, &config) != BK_OK) {
    return H2_PAL_ERR_IO;
  }
  s_button_configured_mask |= 1u << index;
  return H2_PAL_OK;
}

static h2_pal_result_t periph_emit(h2_pal_periph_type_t type_filter,
                                   h2_pal_periph_cb_t cb, void* cb_user,
                                   h2_pal_periph_info_t info) {
  if (type_filter != H2_PAL_PERIPH_TYPE_ANY && info.type != type_filter) {
    return H2_PAL_OK;
  }
  return cb(cb_user, &info);
}

static h2_pal_result_t periph_list(void* user, h2_pal_periph_type_t type_filter,
                                   h2_pal_periph_cb_t cb, void* cb_user) {
  (void)user;

  for (size_t i = 0u; i < sizeof(s_buttons) / sizeof(s_buttons[0]); ++i) {
    h2_pal_periph_info_t info = {
        .id = s_buttons[i].id,
        .type = H2_PAL_PERIPH_TYPE_SINGLE_BUTTON,
    };
    (void)strncpy(info.name, s_buttons[i].name, sizeof(info.name) - 1u);
    h2_pal_result_t rc = periph_emit(type_filter, cb, cb_user, info);
    if (rc != H2_PAL_OK) {
      return rc;
    }
  }

  h2_pal_result_t rc = periph_emit(type_filter, cb, cb_user,
                                   (h2_pal_periph_info_t){
                                       .id = H2_ZERO_BK_NFC_READER_ID,
                                       .type = H2_PAL_PERIPH_TYPE_NFC_READER,
                                       .name = "nfc",
                                   });
  if (rc != H2_PAL_OK) {
    return rc;
  }
  rc = periph_emit(type_filter, cb, cb_user,
                   (h2_pal_periph_info_t){
                       .id = H2_ZERO_BK_BATTERY_ID,
                       .type = H2_PAL_PERIPH_TYPE_BATTERY,
                       .name = "battery",
                   });
  if (rc != H2_PAL_OK) {
    return rc;
  }
  rc = periph_emit(type_filter, cb, cb_user,
                   (h2_pal_periph_info_t){
                       .id = H2_ZERO_BK_LED_STRIP_ID,
                       .type = H2_PAL_PERIPH_TYPE_LED_STRIP,
                       .name = "led_strip",
                       .payload = &s_led_strip_payload,
                       .payload_size = sizeof(s_led_strip_payload),
                   });
  if (rc != H2_PAL_OK) {
    return rc;
  }
  rc = periph_emit(type_filter, cb, cb_user,
                   (h2_pal_periph_info_t){
                       .id = H2_ZERO_BK_SOC_TEMPERATURE_ID,
                       .type = H2_PAL_PERIPH_TYPE_TEMPERATURE_SENSOR,
                       .name = "soc_temp",
                   });
  if (rc != H2_PAL_OK) {
    return rc;
  }
  return periph_emit(type_filter, cb, cb_user,
                     (h2_pal_periph_info_t){
                         .id = H2_ZERO_BK_VIBRATION_ID,
                         .type = H2_PAL_PERIPH_TYPE_PWM_SWITCH,
                         .name = "vibration",
                         .payload = &s_vibration_payload,
                         .payload_size = sizeof(s_vibration_payload),
                     });
}

typedef struct periph_find {
  h2_pal_periph_id_t id;
  h2_pal_periph_info_t info;
} periph_find_t;

static h2_pal_result_t periph_find_cb(void* user,
                                      const h2_pal_periph_info_t* info) {
  periph_find_t* find = (periph_find_t*)user;
  if (find == NULL || info == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (info->id != find->id) {
    return H2_PAL_OK;
  }
  find->info = *info;
  return H2_PAL_ERR_WOULD_BLOCK;
}

static h2_pal_result_t periph_get(void* user, h2_pal_periph_id_t id,
                                  h2_pal_periph_info_t* out_info) {
  (void)user;
  periph_find_t find = {.id = id};
  h2_pal_result_t rc =
      periph_list(NULL, H2_PAL_PERIPH_TYPE_ANY, periph_find_cb, &find);
  if (rc != H2_PAL_ERR_WOULD_BLOCK) {
    return rc == H2_PAL_OK ? H2_PAL_ERR_NOT_FOUND : rc;
  }
  *out_info = find.info;
  return H2_PAL_OK;
}

static h2_pal_result_t read_single_button(
    void* user, h2_pal_periph_id_t id,
    h2_pal_single_button_reading_t* out_reading) {
  (void)user;
  size_t index = 0u;
  const h2_zero_bk_button_t* button = find_button(id, &index);
  if (out_reading == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (button == NULL) {
    return known_periph_id(id) ? H2_PAL_ERR_INVALID_ARG : H2_PAL_ERR_NOT_FOUND;
  }
  h2_pal_result_t rc = configure_button(button, index);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  const bool high = bk_gpio_get_input(button->gpio);
  if (id == H2_ZERO_BK_POWER_BUTTON_ID) {
    static int previous_level = -1;
    static uint32_t previous_config;
    const uint32_t config = bk_gpio_get_value(button->gpio);
    if (previous_level != (int)high || previous_config != config) {
      printf("H2_ZERO_BK_POWER gpio=2 level=%u config=%08lx\n", (unsigned)high,
             (unsigned long)config);
      previous_level = high;
      previous_config = config;
    }
  }
  *out_reading = (h2_pal_single_button_reading_t){
      .id = id,
      .state =
          high ? H2_PAL_BUTTON_STATE_RELEASED : H2_PAL_BUTTON_STATE_PRESSED,
  };
  return H2_PAL_OK;
}

static h2_pal_result_t read_radio_button_group(
    void* user, h2_pal_periph_id_t id,
    h2_pal_radio_button_group_reading_t* out_reading) {
  (void)user;
  if (out_reading == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return known_periph_id(id) ? H2_PAL_ERR_INVALID_ARG : H2_PAL_ERR_NOT_FOUND;
}

static h2_pal_result_t read_temperature(
    void* user, h2_pal_periph_id_t id,
    h2_pal_temperature_reading_t* out_reading) {
  (void)user;
  if (out_reading == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (id != H2_ZERO_BK_SOC_TEMPERATURE_ID) {
    return known_periph_id(id) ? H2_PAL_ERR_INVALID_ARG : H2_PAL_ERR_NOT_FOUND;
  }

  float celsius = 0.0f;
  if (bk_phy_get_current_temperature(&celsius) != BK_OK) {
    return H2_PAL_ERR_IO;
  }

  *out_reading = (h2_pal_temperature_reading_t){
      .id = id,
      .flags = H2_PAL_TEMPERATURE_HAS_MILLI_CELSIUS,
      .milli_celsius = (int32_t)(celsius * 1000.0f),
  };
  return H2_PAL_OK;
}

static h2_pal_result_t read_battery(void* user, h2_pal_periph_id_t id,
                                    h2_pal_battery_reading_t* out_reading) {
  (void)user;
  if (out_reading == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (id != H2_ZERO_BK_BATTERY_ID) {
    return known_periph_id(id) ? H2_PAL_ERR_INVALID_ARG : H2_PAL_ERR_NOT_FOUND;
  }

  uint16_t pin_mv = 0u;
  h2_pal_result_t rc = battery_adc_read_pin_mv(&pin_mv);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  bool usb_inserted = false;
  rc = usb_insert_read(&usb_inserted);
  if (rc != H2_PAL_OK) {
    return rc;
  }

  const int32_t voltage_mv = battery_voltage_mv_from_pin_mv(pin_mv);
  const uint16_t percent_x100 = battery_percent_x100(voltage_mv);
  uint32_t flags = H2_PAL_BATTERY_PRESENT | H2_PAL_BATTERY_HAS_VOLTAGE_MV |
                   H2_PAL_BATTERY_HAS_PERCENT_X100;
  if (voltage_mv <= (int32_t)H2_ZERO_BK_BATTERY_EMPTY_MV) {
    flags |= H2_PAL_BATTERY_LOW;
  }
  if (usb_inserted) {
    flags |= H2_PAL_BATTERY_CHARGING;
    if (percent_x100 >= 10000u) {
      flags |= H2_PAL_BATTERY_FULL;
    }
  }

  *out_reading = (h2_pal_battery_reading_t){
      .id = id,
      .flags = flags,
      .voltage_mv = voltage_mv,
      .percent_x100 = percent_x100,
  };
  return H2_PAL_OK;
}

const h2_pal_button_api_t* h2_bk7258_board_button_api(void) {
  static const h2_pal_button_vtable_t vtable = {
      .read_single_button = read_single_button,
      .read_radio_button_group = read_radio_button_group,
  };
  static const h2_pal_button_api_t api = {
      .vtable = &vtable,
  };
  return &api;
}

h2_pal_input_api_t* h2_bk7258_board_input_api(void) {
  static const h2_pal_input_vtable_t vtable = {
      .read_battery = read_battery,
      .read_temperature = read_temperature,
  };
  static h2_pal_input_api_t api = {
      .user = NULL,
      .vtable = &vtable,
  };
  return &api;
}

const h2_pal_nfc_api_t* h2_bk7258_board_nfc_api(void) {
  if (s_nfc_initialized == 0) {
    if (nfc_board_power_reset() != H2_PAL_OK) return NULL;
    const h2_bk_nfc_fm175xx_config_t config = {
        .id = H2_ZERO_BK_NFC_READER_ID,
        .i2c_id = (i2c_id_t)H2_ZERO_BK_NFC_I2C_ID,
        .i2c_address = H2_ZERO_BK_NFC_I2C_ADDRESS,
        .i2c_speed_hz = H2_ZERO_BK_NFC_I2C_FREQUENCY_HZ,
        .read_capacity = H2_ZERO_BK_NFC_READ_CAPACITY,
    };
    const h2_fm175xx_type_a_profile_t profile = {
        .rf_config = H2_ZERO_BK_NFC_RF_CONFIG,
        .rx_threshold = H2_ZERO_BK_NFC_RX_THRESHOLD,
        .preserve_tx_drive = 1u,
    };
    if (h2_bk_nfc_fm175xx_init_with_profile(&s_nfc, &config, &profile) !=
        H2_PAL_OK) {
      return NULL;
    }
    s_nfc_initialized = 1;
  }
  return h2_bk_nfc_fm175xx_api(&s_nfc);
}

const h2_pal_periph_api_t* h2_bk7258_board_periph_api(void) {
  static const h2_pal_periph_vtable_t vtable = {
      .list = periph_list,
      .get = periph_get,
  };
  static const h2_pal_periph_api_t api = {
      .user = NULL,
      .vtable = &vtable,
  };
  return &api;
}
