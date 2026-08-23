#include "h2_esp_board_private.h"

#include "h2_esp_board_config.h"

#include "esp_err.h"
#include "led_strip.h"

static led_strip_handle_t s_led_strip;
static uint8_t s_brightness_percent = 100u;

static h2_pal_result_t platform_rc_from_esp(esp_err_t err) {
    switch (err) {
        case ESP_OK:
            return H2_PAL_OK;
        case ESP_ERR_INVALID_ARG:
            return H2_PAL_ERR_INVALID_ARG;
        case ESP_ERR_NO_MEM:
            return H2_PAL_ERR_NO_MEMORY;
        case ESP_ERR_INVALID_STATE:
            return H2_PAL_ERR_INVALID_STATE;
        case ESP_ERR_NOT_FOUND:
            return H2_PAL_ERR_NOT_FOUND;
        case ESP_ERR_NOT_SUPPORTED:
            return H2_PAL_ERR_UNSUPPORTED;
        default:
            return H2_PAL_ERR_IO;
    }
}

static h2_pal_result_t validate_led_id(h2_pal_led_id_t id) {
    if (id == H2_DEVKIT_LED_STRIP_ID) {
        return H2_PAL_OK;
    }

    h2_pal_periph_info_t info;
    h2_pal_result_t rc = h2_pal_periph_get(h2_esp_board_periph_api(), id, &info);
    return rc == H2_PAL_OK ? H2_PAL_ERR_INVALID_ARG : rc;
}

static h2_pal_result_t ensure_led_strip(void) {
    if (s_led_strip != NULL) {
        return H2_PAL_OK;
    }

    const led_strip_config_t strip_config = {
        .strip_gpio_num = H2_DEVKIT_LED_STRIP_GPIO,
        .max_leds = H2_DEVKIT_LED_STRIP_PIXEL_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    const led_strip_rmt_config_t rmt_config = {
        .resolution_hz = H2_DEVKIT_LED_STRIP_RMT_RES_HZ,
        .flags.with_dma = H2_DEVKIT_LED_STRIP_RMT_DMA != 0,
    };
    led_strip_handle_t strip = NULL;
    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &strip);
    if (err != ESP_OK) {
        return platform_rc_from_esp(err);
    }
    s_led_strip = strip;
    return H2_PAL_OK;
}

static h2_pal_led_color_t scale_color(h2_pal_led_color_t color) {
    h2_pal_led_color_t out = {
        .r = (uint8_t)(((uint16_t)color.r * s_brightness_percent) / 100u),
        .g = (uint8_t)(((uint16_t)color.g * s_brightness_percent) / 100u),
        .b = (uint8_t)(((uint16_t)color.b * s_brightness_percent) / 100u),
        .w = (uint8_t)(((uint16_t)color.w * s_brightness_percent) / 100u),
    };
    return out;
}

static h2_pal_result_t led_get_info(
    void *user,
    h2_pal_led_id_t id,
    h2_pal_led_info_t *out_info) {
    (void)user;
    if (out_info == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t rc = validate_led_id(id);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    *out_info = (h2_pal_led_info_t){
        .id = id,
        .pixel_count = H2_DEVKIT_LED_STRIP_PIXEL_COUNT,
        .channels_per_pixel = H2_DEVKIT_LED_STRIP_CHANNELS_PER_PIXEL,
        .supports_brightness = 1u,
        .frame_commit_is_atomic = 1u,
    };
    return H2_PAL_OK;
}

static h2_pal_result_t led_set_frame(
    void *user,
    h2_pal_led_id_t id,
    const h2_pal_led_color_t *pixels,
    size_t pixel_count) {
    (void)user;
    h2_pal_result_t rc = validate_led_id(id);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (pixels == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (pixel_count != H2_DEVKIT_LED_STRIP_PIXEL_COUNT) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = ensure_led_strip();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    for (size_t i = 0; i < pixel_count; i++) {
        h2_pal_led_color_t color = scale_color(pixels[i]);
        esp_err_t err = led_strip_set_pixel(s_led_strip, (uint32_t)i, color.r, color.g, color.b);
        if (err != ESP_OK) {
            return platform_rc_from_esp(err);
        }
    }
    return platform_rc_from_esp(led_strip_refresh(s_led_strip));
}

static h2_pal_result_t led_set_solid(
    void *user,
    h2_pal_led_id_t id,
    h2_pal_led_color_t color) {
    (void)user;
    h2_pal_result_t rc = validate_led_id(id);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = ensure_led_strip();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    h2_pal_led_color_t scaled = scale_color(color);
    for (size_t i = 0; i < H2_DEVKIT_LED_STRIP_PIXEL_COUNT; i++) {
        esp_err_t err = led_strip_set_pixel(s_led_strip, (uint32_t)i, scaled.r, scaled.g, scaled.b);
        if (err != ESP_OK) {
            return platform_rc_from_esp(err);
        }
    }
    return platform_rc_from_esp(led_strip_refresh(s_led_strip));
}

static h2_pal_result_t led_clear(void *user, h2_pal_led_id_t id) {
    (void)user;
    h2_pal_result_t rc = validate_led_id(id);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = ensure_led_strip();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return platform_rc_from_esp(led_strip_clear(s_led_strip));
}

static h2_pal_result_t led_set_brightness_percent(
    void *user,
    h2_pal_led_id_t id,
    uint8_t percent) {
    (void)user;
    h2_pal_result_t rc = validate_led_id(id);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (percent > 100u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    s_brightness_percent = percent;
    return H2_PAL_OK;
}

const h2_pal_led_api_t *h2_esp_board_led_api(void) {
    static const h2_pal_led_vtable_t vtable = {
        .get_info = led_get_info,
        .set_frame = led_set_frame,
        .set_solid = led_set_solid,
        .clear = led_clear,
        .set_brightness_percent = led_set_brightness_percent,
    };
    static const h2_pal_led_api_t api = {
        .user = NULL,
        .vtable = &vtable,
    };
    return &api;
}
