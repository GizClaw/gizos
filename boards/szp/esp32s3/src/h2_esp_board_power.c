#include "h2_esp_board_private.h"
#include "h2_esp_szp_board_internal.h"

#include "h2_esp_platform_core.h"

#include "driver/gpio.h"

#include <stdint.h>

#define H2_SZP_DISPLAY_SCLK_GPIO GPIO_NUM_41
#define H2_SZP_DISPLAY_MOSI_GPIO GPIO_NUM_40
#define H2_SZP_DISPLAY_DC_GPIO GPIO_NUM_39
#define H2_SZP_DISPLAY_BL_GPIO GPIO_NUM_42

#define H2_SZP_AUDIO_MCLK_GPIO GPIO_NUM_38
#define H2_SZP_AUDIO_BCLK_GPIO GPIO_NUM_14
#define H2_SZP_AUDIO_WS_GPIO GPIO_NUM_13
#define H2_SZP_AUDIO_DOUT_GPIO GPIO_NUM_45
#define H2_SZP_AUDIO_DIN_GPIO GPIO_NUM_12

static void drive_gpio_group_low(uint64_t mask) {
    const gpio_config_t config = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (mask == 0u || gpio_config(&config) != ESP_OK) {
        return;
    }
    for (int gpio = 0; gpio < GPIO_NUM_MAX && gpio < 64; ++gpio) {
        if ((mask & (1ULL << (uint32_t)gpio)) != 0u) {
            (void)gpio_set_level((gpio_num_t)gpio, 0);
        }
    }
}

h2_pal_result_t h2_esp_platform_power_before_reboot(uint32_t reason) {
    (void)reason;

    (void)h2_esp_board_display_power_off();

    h2_pal_audio_t *audio = h2_esp_board_audio_if_initialized();
    if (audio != NULL) {
        (void)h2_pal_audio_stop_mic(audio);
        (void)h2_pal_audio_stop_speaker(audio);
    }
    (void)h2_esp_szp_board_set_pa(0);

    drive_gpio_group_low(
        (1ULL << H2_SZP_DISPLAY_SCLK_GPIO) |
        (1ULL << H2_SZP_DISPLAY_MOSI_GPIO) |
        (1ULL << H2_SZP_DISPLAY_DC_GPIO) |
        (1ULL << H2_SZP_DISPLAY_BL_GPIO));
    drive_gpio_group_low(
        (1ULL << H2_SZP_AUDIO_MCLK_GPIO) |
        (1ULL << H2_SZP_AUDIO_BCLK_GPIO) |
        (1ULL << H2_SZP_AUDIO_WS_GPIO) |
        (1ULL << H2_SZP_AUDIO_DOUT_GPIO) |
        (1ULL << H2_SZP_AUDIO_DIN_GPIO));
    return H2_PAL_OK;
}

const h2_pal_power_api_t *h2_esp_board_power_api(void) {
    return h2_esp_platform_power_api();
}
