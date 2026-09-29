#ifndef H2_BK7258_DISPLAY_BACKLIGHT_H
#define H2_BK7258_DISPLAY_BACKLIGHT_H

#include "h2/pal/hal/h2_pal_display.h"
#include <stdbool.h>
#include <stdint.h>
#ifdef H2_BK7258_DISPLAY_BACKLIGHT_HOST_TEST
#include "h2_bk7258_display_backlight_test_sdk.h"
#else
#include "driver/pwm.h"
#endif

typedef struct h2_bk7258_backlight_state {
    bool running;
    bool needs_cleanup;
} h2_bk7258_backlight_state_t;

static int h2_bk7258_backlight_release(h2_bk7258_backlight_state_t *state) {
    if (state->running || state->needs_cleanup) {
        if (bk_pwm_deinit(PWM_CH_1) != BK_OK) {
            state->running = false;
            state->needs_cleanup = true;
            return H2_DISPLAY_ERR_IO;
        }
    }
    state->running = false;
    state->needs_cleanup = false;
    return H2_DISPLAY_OK;
}

static int h2_bk7258_backlight_pwm(h2_bk7258_backlight_state_t *state,
                                  uint32_t percent) {
    if (percent == 0u || percent >= 100u)
        return H2_DISPLAY_ERR_INVALID_ARG;
    if (state->needs_cleanup) {
        int rc = h2_bk7258_backlight_release(state);
        if (rc) return rc;
    }
    /* 26MHz / 26000 = 1kHz. Board GPIO map assigns PWM1 to GPIO7,
     * preserving LCD_R7 on the SDK's default PWM1 pin GPIO19. */
    const uint32_t period = 26000u;
    if (!state->running) {
        if (bk_pwm_driver_init() != BK_OK) return H2_DISPLAY_ERR_IO;
        const pwm_init_config_t config = {
            .period_cycle = period, .duty_cycle = period * percent / 100u};
        /* Initialization may have acquired resources before reporting an
         * error. Retain cleanup ownership until native deinit succeeds. */
        state->needs_cleanup = true;
        if (bk_pwm_init(PWM_CH_1, &config) != BK_OK ||
            bk_pwm_start(PWM_CH_1) != BK_OK) {
            (void)h2_bk7258_backlight_release(state);
            return H2_DISPLAY_ERR_IO;
        }
        state->running = true;
        state->needs_cleanup = false;
    } else {
        pwm_period_duty_config_t config = {
            .period_cycle = period, .duty_cycle = period * percent / 100u};
        if (bk_pwm_set_period_duty(PWM_CH_1, &config) != BK_OK)
            return H2_DISPLAY_ERR_IO;
    }
    return H2_DISPLAY_OK;
}
#endif
