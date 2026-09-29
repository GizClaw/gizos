#include "h2_bk7258_display_backlight.h"
#include <assert.h>
#include <string.h>

static struct {
    int driver_rc, init_rc, start_rc, cleanup_rc, update_rc;
    unsigned driver_calls, init_calls, start_calls, cleanup_calls, update_calls;
    uint32_t duty;
} native;
bk_err_t bk_pwm_driver_init(void) { ++native.driver_calls; return native.driver_rc; }
bk_err_t bk_pwm_init(pwm_chan_t channel, const pwm_init_config_t *config) {
    assert(channel == PWM_CH_1 && config->period_cycle == 26000u);
    native.duty = config->duty_cycle;
    ++native.init_calls;
    return native.init_rc;
}
bk_err_t bk_pwm_start(pwm_chan_t channel) {
    assert(channel == PWM_CH_1);
    ++native.start_calls;
    return native.start_rc;
}
bk_err_t bk_pwm_deinit(pwm_chan_t channel) {
    assert(channel == PWM_CH_1);
    ++native.cleanup_calls;
    return native.cleanup_rc;
}
bk_err_t bk_pwm_set_period_duty(pwm_chan_t channel, const pwm_period_duty_config_t *config) {
    assert(channel == PWM_CH_1 && config->period_cycle == 26000u);
    native.duty = config->duty_cycle;
    ++native.update_calls;
    return native.update_rc;
}
static void reset(void) { memset(&native, 0, sizeof(native)); }
int main(void) {
    h2_bk7258_backlight_state_t state = {0};
    reset();
    native.driver_rc = -1;
    assert(h2_bk7258_backlight_pwm(&state, 50) == H2_DISPLAY_ERR_IO);
    assert(!state.running && !state.needs_cleanup);
    assert(!native.init_calls && !native.cleanup_calls);
    native.driver_rc = 0;
    assert(h2_bk7258_backlight_pwm(&state, 50) == 0);
    assert(state.running && native.start_calls == 1 && native.duty == 13000u);
    assert(h2_bk7258_backlight_release(&state) == 0);

    reset();
    native.init_rc = -1;
    assert(h2_bk7258_backlight_pwm(&state, 50) == H2_DISPLAY_ERR_IO);
    assert(!state.running && !state.needs_cleanup);
    assert(native.cleanup_calls == 1 && !native.start_calls);
    native.init_rc = 0;
    assert(h2_bk7258_backlight_pwm(&state, 25) == 0);
    assert(native.init_calls == 2 && native.start_calls == 1 && !native.update_calls);
    assert(h2_bk7258_backlight_release(&state) == 0);

    reset();
    native.start_rc = -1;
    assert(h2_bk7258_backlight_pwm(&state, 50) == H2_DISPLAY_ERR_IO);
    assert(!state.running && !state.needs_cleanup && native.cleanup_calls == 1);
    native.start_rc = 0;
    assert(h2_bk7258_backlight_pwm(&state, 75) == 0);
    assert(native.init_calls == 2 && native.start_calls == 2 && !native.update_calls);
    assert(h2_bk7258_backlight_release(&state) == 0);

    reset();
    native.start_rc = native.cleanup_rc = -1;
    assert(h2_bk7258_backlight_pwm(&state, 50) == H2_DISPLAY_ERR_IO);
    assert(!state.running && state.needs_cleanup);
    native.start_rc = 0;
    assert(h2_bk7258_backlight_pwm(&state, 75) == H2_DISPLAY_ERR_IO);
    assert(native.init_calls == 1 && native.start_calls == 1 && !native.update_calls);
    native.cleanup_rc = 0;
    assert(h2_bk7258_backlight_pwm(&state, 75) == 0);
    assert(state.running && !state.needs_cleanup && native.init_calls == 2 && native.start_calls == 2);
    assert(h2_bk7258_backlight_pwm(&state, 25) == 0 && native.duty == 6500u);
    native.update_rc = -1;
    assert(h2_bk7258_backlight_pwm(&state, 50) == H2_DISPLAY_ERR_IO);
    assert(state.running);
    native.update_rc = 0;
    assert(h2_bk7258_backlight_pwm(&state, 50) == 0);
    native.cleanup_rc = -1;
    assert(h2_bk7258_backlight_release(&state) == H2_DISPLAY_ERR_IO);
    assert(!state.running && state.needs_cleanup);
    native.cleanup_rc = 0;
    assert(h2_bk7258_backlight_release(&state) == 0);
    assert(h2_bk7258_backlight_release(&state) == 0);
    assert(h2_bk7258_backlight_pwm(&state, 0) == H2_DISPLAY_ERR_INVALID_ARG);
    assert(h2_bk7258_backlight_pwm(&state, 100) == H2_DISPLAY_ERR_INVALID_ARG);
    assert(h2_bk7258_backlight_pwm(&state, 101) == H2_DISPLAY_ERR_INVALID_ARG);
    return 0;
}
