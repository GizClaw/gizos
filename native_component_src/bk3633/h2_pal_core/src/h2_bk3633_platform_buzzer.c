#include "h2_bk3633_platform_core.h"

#include <stdbool.h>
#include <string.h>

#define H2_BK3633_PWM_CLOCK_HZ 16000000u

#if defined(H2_BK3633_INTERACTION_SDK_FAKE)
#include "h2_bk3633_interaction_sdk_fake.h"
#else
#include "BK3633_RegList.h"
#include "pwm.h"

static h2_pal_result_t
h2_bk3633_interaction_sdk_pwm_initialize(uint8_t block, uint8_t channel,
                                         bool continuous_mode,
                                         uint32_t end_value,
                                         uint32_t duty_cycle) {
  PWM_DRV_DESC descriptor = {
      .channel = channel,
      .en = 1u,
      .int_en = 0u,
      .mode = PWM_MODE_PWM,
      .cpedg_sel = 1u,
      .contiu_mode = continuous_mode ? 1u : 0u,
      .clk_src = PWM_CLK_XTAL16M,
      .pre_divid = 0u,
      .end_value = end_value,
      .duty_cycle = duty_cycle,
      .p_Int_Handler = NULL,
  };

  if (block == 0u) {
    pwm0_init(&descriptor);
  } else {
    pwm1_init(&descriptor);
  }
  return H2_PAL_OK;
}

static h2_pal_result_t
h2_bk3633_interaction_sdk_pwm_update(uint8_t block, uint8_t channel,
                                     uint32_t end_value,
                                     uint32_t duty_cycle) {
  if (block == 0u) {
    switch (channel) {
    case 0u:
      addPWM0_Reg0x2 = end_value;
      addPWM0_Reg0x3 = duty_cycle;
      break;
    case 1u:
      addPWM0_Reg0x5 = end_value;
      addPWM0_Reg0x6 = duty_cycle;
      break;
    case 2u:
      addPWM0_Reg0x8 = end_value;
      addPWM0_Reg0x9 = duty_cycle;
      break;
    default:
      return H2_PAL_ERR_INVALID_ARG;
    }
  } else if (block == 1u) {
    pwm1_end_value_duty_cycle_set(channel, end_value, duty_cycle);
  } else {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return H2_PAL_OK;
}
#endif

struct h2_bk3633_platform_buzzer {
  h2_pal_buzzer_api_t api;
  const h2_pal_mem_api_t *mem;
  h2_bk3633_platform_buzzer_config_t config;
  bool running;
};

static uint8_t
buzzer_expected_gpio(const h2_bk3633_platform_pwm_channel_t *channel) {
  return (uint8_t)(0x10u + channel->block * H2_BK3633_PWM_CHANNEL_COUNT +
                   channel->channel);
}

static bool
buzzer_channel_valid(const h2_bk3633_platform_pwm_channel_t *channel) {
  return channel->block <= 1u &&
         channel->channel < H2_BK3633_PWM_CHANNEL_COUNT &&
         channel->gpio_pin == buzzer_expected_gpio(channel);
}

static h2_pal_result_t
buzzer_config_validate(const h2_bk3633_platform_buzzer_config_t *config,
                       const h2_pal_mem_api_t *mem,
                       h2_bk3633_platform_buzzer_t **out_buzzer) {
  if (config == NULL || mem == NULL || out_buzzer == NULL ||
      config->periph_id == 0u || !buzzer_channel_valid(&config->frequency) ||
      !buzzer_channel_valid(&config->volume) ||
      (config->frequency.block == config->volume.block &&
       config->frequency.channel == config->volume.channel) ||
      config->min_frequency_hz == 0u ||
      config->min_frequency_hz > config->max_frequency_hz ||
      config->max_frequency_hz > H2_BK3633_PWM_CLOCK_HZ / 2u ||
      config->volume_pwm_frequency_hz == 0u ||
      config->volume_pwm_frequency_hz > H2_BK3633_PWM_CLOCK_HZ / 2u ||
      config->volume_inverted > 1u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t buzzer_channel_values(uint32_t frequency_hz,
                                             uint8_t duty_percent,
                                             uint32_t *out_end_value,
                                             uint32_t *out_duty_cycle) {
  uint32_t end_value;

  if (frequency_hz == 0u || duty_percent > 100u || out_end_value == NULL ||
      out_duty_cycle == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  end_value = H2_BK3633_PWM_CLOCK_HZ / frequency_hz;
  if (end_value < 2u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_end_value = end_value;
  *out_duty_cycle =
      (uint32_t)(((uint64_t)end_value * duty_percent) / 100u);
  return H2_PAL_OK;
}

static h2_pal_result_t
buzzer_initialize_channel(const h2_bk3633_platform_pwm_channel_t *channel,
                          uint32_t frequency_hz, uint8_t duty_percent) {
  uint32_t end_value;
  uint32_t duty_cycle;
  h2_pal_result_t rc = buzzer_channel_values(
      frequency_hz, duty_percent, &end_value, &duty_cycle);

  if (rc != H2_PAL_OK) {
    return rc;
  }
  return h2_bk3633_interaction_sdk_pwm_initialize(
      channel->block, channel->channel, channel->continuous_mode, end_value,
      duty_cycle);
}

static h2_pal_result_t
buzzer_update_channel(const h2_bk3633_platform_pwm_channel_t *channel,
                      uint32_t frequency_hz, uint8_t duty_percent) {
  uint32_t end_value;
  uint32_t duty_cycle;
  h2_pal_result_t rc = buzzer_channel_values(
      frequency_hz, duty_percent, &end_value, &duty_cycle);

  if (rc != H2_PAL_OK) {
    return rc;
  }
  return h2_bk3633_interaction_sdk_pwm_update(
      channel->block, channel->channel, end_value, duty_cycle);
}

static h2_pal_result_t buzzer_silence(h2_bk3633_platform_buzzer_t *buzzer) {
  h2_pal_result_t first = buzzer_update_channel(
      &buzzer->config.frequency, buzzer->config.min_frequency_hz, 0u);
  uint8_t safe_volume = buzzer->config.volume_inverted != 0u ? 100u : 0u;
  h2_pal_result_t rc = buzzer_update_channel(
      &buzzer->config.volume, buzzer->config.volume_pwm_frequency_hz,
      safe_volume);

  buzzer->running = false;
  return first != H2_PAL_OK ? first : rc;
}

static h2_pal_result_t buzzer_get_info(void *user, h2_pal_buzzer_id_t id,
                                       h2_pal_buzzer_info_t *out_info) {
  h2_bk3633_platform_buzzer_t *buzzer = (h2_bk3633_platform_buzzer_t *)user;

  *out_info = (h2_pal_buzzer_info_t){0};
  if (buzzer == NULL || id != buzzer->config.periph_id) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  *out_info = (h2_pal_buzzer_info_t){
      .id = id,
      .min_frequency_hz = buzzer->config.min_frequency_hz,
      .max_frequency_hz = buzzer->config.max_frequency_hz,
      .supports_volume = 1u,
  };
  return H2_PAL_OK;
}

static h2_pal_result_t buzzer_start(void *user, h2_pal_buzzer_id_t id,
                                    uint32_t frequency_hz,
                                    uint8_t volume_percent) {
  h2_bk3633_platform_buzzer_t *buzzer = (h2_bk3633_platform_buzzer_t *)user;
  uint8_t hardware_volume;
  h2_pal_result_t rc;

  if (buzzer == NULL || id != buzzer->config.periph_id) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  if (frequency_hz < buzzer->config.min_frequency_hz ||
      frequency_hz > buzzer->config.max_frequency_hz ||
      volume_percent > H2_PAL_BUZZER_VOLUME_PERCENT_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  hardware_volume = buzzer->config.volume_inverted != 0u
                        ? (uint8_t)(100u - volume_percent)
                        : volume_percent;
  rc = buzzer_update_channel(&buzzer->config.volume,
                             buzzer->config.volume_pwm_frequency_hz,
                             hardware_volume);
  if (rc != H2_PAL_OK) {
    (void)buzzer_silence(buzzer);
    return rc;
  }
  rc = buzzer_update_channel(&buzzer->config.frequency, frequency_hz, 50u);
  if (rc != H2_PAL_OK) {
    (void)buzzer_silence(buzzer);
    return rc;
  }
  buzzer->running = true;
  return H2_PAL_OK;
}

static h2_pal_result_t buzzer_stop(void *user, h2_pal_buzzer_id_t id) {
  h2_bk3633_platform_buzzer_t *buzzer = (h2_bk3633_platform_buzzer_t *)user;

  if (buzzer == NULL || id != buzzer->config.periph_id) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  return buzzer_silence(buzzer);
}

static const h2_pal_buzzer_vtable_t s_buzzer_vtable = {
    .get_info = buzzer_get_info,
    .start = buzzer_start,
    .stop = buzzer_stop,
};

h2_pal_result_t
h2_bk3633_platform_buzzer_init(const h2_bk3633_platform_buzzer_config_t *config,
                               const h2_pal_mem_api_t *mem,
                               h2_bk3633_platform_buzzer_t **out_buzzer) {
  h2_bk3633_platform_buzzer_t *buzzer;
  h2_pal_result_t rc = buzzer_config_validate(config, mem, out_buzzer);
  if (out_buzzer != NULL) {
    *out_buzzer = NULL;
  }
  if (rc != H2_PAL_OK) {
    return rc;
  }
  buzzer = h2_pal_mem_alloc(mem, sizeof(*buzzer));
  if (buzzer == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  memset(buzzer, 0, sizeof(*buzzer));
  buzzer->api = (h2_pal_buzzer_api_t){
      .user = buzzer,
      .vtable = &s_buzzer_vtable,
  };
  buzzer->mem = mem;
  buzzer->config = *config;
  rc = buzzer_initialize_channel(&buzzer->config.frequency,
                                 buzzer->config.min_frequency_hz, 0u);
  if (rc != H2_PAL_OK) {
    h2_pal_mem_free(mem, buzzer);
    return rc;
  }
  rc = buzzer_initialize_channel(
      &buzzer->config.volume, buzzer->config.volume_pwm_frequency_hz,
      buzzer->config.volume_inverted != 0u ? 100u : 0u);
  if (rc != H2_PAL_OK) {
    (void)buzzer_update_channel(&buzzer->config.frequency,
                                buzzer->config.min_frequency_hz, 0u);
    h2_pal_mem_free(mem, buzzer);
    return rc;
  }
  *out_buzzer = buzzer;
  return H2_PAL_OK;
}

const h2_pal_buzzer_api_t *
h2_bk3633_platform_buzzer_api(h2_bk3633_platform_buzzer_t *buzzer) {
  return buzzer != NULL ? &buzzer->api : NULL;
}

void h2_bk3633_platform_buzzer_deinit(h2_bk3633_platform_buzzer_t *buzzer) {
  if (buzzer == NULL) {
    return;
  }
  (void)buzzer_silence(buzzer);
  h2_pal_mem_free(buzzer->mem, buzzer);
}
