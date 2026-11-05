#include "h2_bk7258_board_private.h"
#include "h2_zero_bk_1_0_board_config.h"

#include <common/bk_err.h>
#include <driver/pwm.h>
#include <stdint.h>

static int s_pwm_initialized;
static int s_pwm_started;
static uint16_t s_duty_x100;

static h2_pal_result_t validate_vibration_id(h2_pal_periph_id_t id) {
  if (id == H2_ZERO_BK_1_0_VIBRATION_ID) {
    return H2_PAL_OK;
  }
  h2_pal_periph_info_t info;
  h2_pal_result_t rc =
      h2_pal_periph_get(h2_bk7258_board_periph_api(), id, &info);
  return rc == H2_PAL_OK ? H2_PAL_ERR_INVALID_ARG : rc;
}

static h2_pal_result_t ensure_pwm(void) {
  if (s_pwm_initialized != 0) {
    return H2_PAL_OK;
  }
  const pwm_init_config_t config = {
      .period_cycle = H2_ZERO_BK_1_0_VIBRATION_PERIOD_CYCLES,
      .duty_cycle = 0u,
      .psc = 0u,
  };
  if (bk_pwm_driver_init() != BK_OK ||
      bk_pwm_init((pwm_chan_t)H2_ZERO_BK_1_0_VIBRATION_PWM_ID, &config) !=
          BK_OK) {
    return H2_PAL_ERR_IO;
  }
  s_pwm_initialized = 1;
  return H2_PAL_OK;
}

static h2_pal_result_t set_duty(void* user, h2_pal_periph_id_t id,
                                uint16_t duty_x100) {
  (void)user;
  h2_pal_result_t rc = validate_vibration_id(id);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  if (duty_x100 > H2_PAL_PWM_SWITCH_DUTY_MAX_X100) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  rc = ensure_pwm();
  if (rc != H2_PAL_OK) {
    return rc;
  }
  pwm_period_duty_config_t config = {
      .period_cycle = H2_ZERO_BK_1_0_VIBRATION_PERIOD_CYCLES,
      .duty_cycle =
          ((uint32_t)duty_x100 * H2_ZERO_BK_1_0_VIBRATION_PERIOD_CYCLES +
           5000u) /
          10000u,
      .psc = 0u,
  };
  if (bk_pwm_set_period_duty((pwm_chan_t)H2_ZERO_BK_1_0_VIBRATION_PWM_ID,
                             &config) != BK_OK) {
    return H2_PAL_ERR_IO;
  }
  if (duty_x100 == 0u) {
    if (s_pwm_started != 0 &&
        bk_pwm_stop((pwm_chan_t)H2_ZERO_BK_1_0_VIBRATION_PWM_ID) != BK_OK) {
      return H2_PAL_ERR_IO;
    }
    s_pwm_started = 0;
  } else if (s_pwm_started == 0) {
    if (bk_pwm_start((pwm_chan_t)H2_ZERO_BK_1_0_VIBRATION_PWM_ID) != BK_OK) {
      return H2_PAL_ERR_IO;
    }
    s_pwm_started = 1;
  }
  s_duty_x100 = duty_x100;
  return H2_PAL_OK;
}

static h2_pal_result_t get_duty(void* user, h2_pal_periph_id_t id,
                                uint16_t* out_duty_x100) {
  (void)user;
  if (out_duty_x100 == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_pal_result_t rc = validate_vibration_id(id);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  *out_duty_x100 = s_duty_x100;
  return H2_PAL_OK;
}

h2_pal_pwm_switch_api_t* h2_bk7258_board_pwm_switch_api(void) {
  static const h2_pal_pwm_switch_vtable_t vtable = {
      .set_duty = set_duty,
      .get_duty = get_duty,
  };
  static h2_pal_pwm_switch_api_t api = {
      .user = NULL,
      .vtable = &vtable,
  };
  return &api;
}
