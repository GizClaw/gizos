#include "h2_bk3633_interaction_sdk_fake.h"
#include "h2_bk3633_mem_test_support.h"
#include "h2_bk3633_platform_core.h"

#include <assert.h>

#define BUZZER_ID 21u

static const h2_bk3633_platform_buzzer_config_t s_config = {
    .periph_id = BUZZER_ID,
    .frequency =
        {
            .block = 0u,
            .channel = 2u,
            .gpio_pin = 0x12u,
            .continuous_mode = false,
        },
    .volume =
        {
            .block = 1u,
            .channel = 0u,
            .gpio_pin = 0x13u,
            .continuous_mode = true,
        },
    .min_frequency_hz = 100u,
    .max_frequency_hz = 5000u,
    .volume_pwm_frequency_hz = 20000u,
    .volume_inverted = 1u,
};

static h2_bk3633_platform_buzzer_t *create_buzzer(void) {
  h2_bk3633_platform_buzzer_t *buzzer = NULL;
  assert(h2_bk3633_platform_buzzer_init(&s_config, h2_bk3633_platform_mem_api(),
                                        &buzzer) == H2_PAL_OK);
  assert(buzzer != NULL);
  return buzzer;
}

static void assert_call(size_t index, uint8_t block, uint8_t channel,
                        bool initialize, bool continuous_mode,
                        uint32_t end_value, uint32_t duty_cycle) {
  const h2_bk3633_interaction_pwm_call_t *call =
      h2_bk3633_interaction_sdk_fake_pwm_call(index);
  assert(call != NULL);
  assert(call->block == block);
  assert(call->channel == channel);
  assert(call->initialize == initialize);
  assert(call->continuous_mode == continuous_mode);
  assert(call->end_value == end_value);
  assert(call->duty_cycle == duty_cycle);
}

static void test_info_start_replace_and_stop(void) {
  h2_bk3633_interaction_sdk_fake_reset();
  h2_bk3633_platform_buzzer_t *buzzer = create_buzzer();
  const h2_pal_buzzer_api_t *api = h2_bk3633_platform_buzzer_api(buzzer);
  h2_pal_buzzer_info_t info;

  assert(h2_pal_buzzer_get_info(api, BUZZER_ID, &info) == H2_PAL_OK);
  assert(info.id == BUZZER_ID);
  assert(info.min_frequency_hz == 100u);
  assert(info.max_frequency_hz == 5000u);
  assert(info.supports_volume == 1u);
  assert(h2_pal_buzzer_start(api, BUZZER_ID, 1000u, 25u) == H2_PAL_OK);
  assert_call(0u, 0u, 2u, true, false, 160000u, 0u);
  assert_call(1u, 1u, 0u, true, true, 800u, 800u);
  assert_call(2u, 1u, 0u, false, false, 800u, 600u);
  assert_call(3u, 0u, 2u, false, false, 16000u, 8000u);

  assert(h2_pal_buzzer_start(api, BUZZER_ID, 2000u, 100u) == H2_PAL_OK);
  assert_call(4u, 1u, 0u, false, false, 800u, 0u);
  assert_call(5u, 0u, 2u, false, false, 8000u, 4000u);
  assert(h2_pal_buzzer_stop(api, BUZZER_ID) == H2_PAL_OK);
  assert_call(6u, 0u, 2u, false, false, 160000u, 0u);
  assert_call(7u, 1u, 0u, false, false, 800u, 800u);
  assert(h2_pal_buzzer_stop(api, BUZZER_ID) == H2_PAL_OK);
  assert(h2_bk3633_interaction_sdk_fake_pwm_call_count() == 10u);
  h2_bk3633_platform_buzzer_deinit(buzzer);
  assert(h2_bk3633_interaction_sdk_fake_pwm_call_count() == 12u);
}

static void test_validation_and_zero_volume(void) {
  h2_bk3633_interaction_sdk_fake_reset();
  h2_bk3633_platform_buzzer_t *buzzer = create_buzzer();
  const h2_pal_buzzer_api_t *api = h2_bk3633_platform_buzzer_api(buzzer);

  assert(h2_pal_buzzer_start(api, BUZZER_ID, 99u, 50u) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_pal_buzzer_start(api, BUZZER_ID, 5001u, 50u) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_pal_buzzer_start(api, 999u, 1000u, 50u) == H2_PAL_ERR_NOT_FOUND);
  assert(h2_pal_buzzer_start(api, BUZZER_ID, 1000u, 0u) == H2_PAL_OK);
  assert(h2_bk3633_interaction_sdk_fake_pwm_call_count() == 4u);
  assert_call(2u, 1u, 0u, false, false, 800u, 800u);
  assert_call(3u, 0u, 2u, false, false, 16000u, 8000u);
  h2_bk3633_platform_buzzer_deinit(buzzer);
}

static void test_noninverted_safe_volume(void) {
  h2_bk3633_platform_buzzer_config_t config = s_config;
  h2_bk3633_platform_buzzer_t *buzzer = NULL;

  config.volume_inverted = 0u;
  h2_bk3633_interaction_sdk_fake_reset();
  assert(h2_bk3633_platform_buzzer_init(&config, h2_bk3633_platform_mem_api(),
                                        &buzzer) == H2_PAL_OK);
  assert_call(0u, 0u, 2u, true, false, 160000u, 0u);
  assert_call(1u, 1u, 0u, true, true, 800u, 0u);
  assert(h2_pal_buzzer_start(h2_bk3633_platform_buzzer_api(buzzer), BUZZER_ID,
                             1000u, 25u) == H2_PAL_OK);
  assert_call(2u, 1u, 0u, false, false, 800u, 200u);
  assert_call(3u, 0u, 2u, false, false, 16000u, 8000u);
  assert(h2_pal_buzzer_stop(h2_bk3633_platform_buzzer_api(buzzer), BUZZER_ID) ==
         H2_PAL_OK);
  assert_call(4u, 0u, 2u, false, false, 160000u, 0u);
  assert_call(5u, 1u, 0u, false, false, 800u, 0u);
  h2_bk3633_platform_buzzer_deinit(buzzer);
}

static void test_partial_failure_silences_both_channels(void) {
  h2_bk3633_interaction_sdk_fake_reset();
  h2_bk3633_platform_buzzer_t *buzzer = create_buzzer();
  const h2_pal_buzzer_api_t *api = h2_bk3633_platform_buzzer_api(buzzer);

  h2_bk3633_interaction_sdk_fake_fail_pwm_call(3u, H2_PAL_ERR_IO);
  assert(h2_pal_buzzer_start(api, BUZZER_ID, 1000u, 50u) == H2_PAL_ERR_IO);
  assert_call(2u, 1u, 0u, false, false, 800u, 400u);
  assert_call(3u, 0u, 2u, false, false, 160000u, 0u);
  assert_call(4u, 1u, 0u, false, false, 800u, 800u);
  h2_bk3633_platform_buzzer_deinit(buzzer);

  h2_bk3633_interaction_sdk_fake_reset();
  buzzer = create_buzzer();
  api = h2_bk3633_platform_buzzer_api(buzzer);
  h2_bk3633_interaction_sdk_fake_fail_pwm_call(4u, H2_PAL_ERR_IO);
  assert(h2_pal_buzzer_start(api, BUZZER_ID, 1000u, 50u) == H2_PAL_ERR_IO);
  assert_call(2u, 1u, 0u, false, false, 800u, 400u);
  assert_call(3u, 0u, 2u, false, false, 16000u, 8000u);
  assert_call(4u, 0u, 2u, false, false, 160000u, 0u);
  assert_call(5u, 1u, 0u, false, false, 800u, 800u);
  h2_bk3633_platform_buzzer_deinit(buzzer);

  h2_bk3633_interaction_sdk_fake_reset();
  h2_bk3633_interaction_sdk_fake_fail_pwm_call(1u, H2_PAL_ERR_IO);
  buzzer = (h2_bk3633_platform_buzzer_t *)(uintptr_t)1u;
  assert(h2_bk3633_platform_buzzer_init(&s_config, h2_bk3633_platform_mem_api(),
                                        &buzzer) == H2_PAL_ERR_IO);
  assert(buzzer == NULL);
  assert_call(0u, 0u, 2u, true, false, 160000u, 0u);

  h2_bk3633_interaction_sdk_fake_reset();
  h2_bk3633_interaction_sdk_fake_fail_pwm_call(2u, H2_PAL_ERR_IO);
  assert(h2_bk3633_platform_buzzer_init(&s_config, h2_bk3633_platform_mem_api(),
                                        &buzzer) == H2_PAL_ERR_IO);
  assert(buzzer == NULL);
  assert_call(0u, 0u, 2u, true, false, 160000u, 0u);
  assert_call(1u, 1u, 0u, true, true, 800u, 800u);
  assert_call(2u, 0u, 2u, false, false, 160000u, 0u);
}

static void test_stop_failure_attempts_both_safe_outputs(void) {
  h2_bk3633_interaction_sdk_fake_reset();
  h2_bk3633_platform_buzzer_t *buzzer = create_buzzer();
  const h2_pal_buzzer_api_t *api = h2_bk3633_platform_buzzer_api(buzzer);

  h2_bk3633_interaction_sdk_fake_fail_pwm_call(3u, H2_PAL_ERR_IO);
  assert(h2_pal_buzzer_stop(api, BUZZER_ID) == H2_PAL_ERR_IO);
  assert_call(2u, 0u, 2u, false, false, 160000u, 0u);
  assert_call(3u, 1u, 0u, false, false, 800u, 800u);
  h2_bk3633_platform_buzzer_deinit(buzzer);

  h2_bk3633_interaction_sdk_fake_reset();
  buzzer = create_buzzer();
  api = h2_bk3633_platform_buzzer_api(buzzer);
  h2_bk3633_interaction_sdk_fake_fail_pwm_call(4u, H2_PAL_ERR_IO);
  assert(h2_pal_buzzer_stop(api, BUZZER_ID) == H2_PAL_ERR_IO);
  assert_call(2u, 0u, 2u, false, false, 160000u, 0u);
  assert_call(3u, 1u, 0u, false, false, 800u, 800u);
  h2_bk3633_platform_buzzer_deinit(buzzer);
}

static void test_rejects_wrong_fixed_pin_mapping(void) {
  h2_bk3633_platform_buzzer_config_t invalid = s_config;
  h2_bk3633_platform_buzzer_t *buzzer = NULL;

  h2_bk3633_interaction_sdk_fake_reset();
  invalid.frequency.gpio_pin = 0x11u;
  assert(h2_bk3633_platform_buzzer_init(&invalid, h2_bk3633_platform_mem_api(),
                                        &buzzer) == H2_PAL_ERR_INVALID_ARG);
  assert(buzzer == NULL);
}

int main(void) {
  h2_bk3633_mem_test_support_init();
  test_info_start_replace_and_stop();
  test_validation_and_zero_volume();
  test_noninverted_safe_volume();
  test_partial_failure_silences_both_channels();
  test_stop_failure_attempts_both_safe_outputs();
  test_rejects_wrong_fixed_pin_mapping();
  return 0;
}
