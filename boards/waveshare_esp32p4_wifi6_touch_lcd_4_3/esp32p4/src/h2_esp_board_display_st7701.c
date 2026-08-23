#include "h2_esp_board_private.h"

#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7701.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LCD_WIDTH 480
#define LCD_HEIGHT 800
#define LCD_BACKLIGHT_GPIO 26
#define LCD_RESET_GPIO 27
#define LCD_DSI_BUS_ID 0
#define LCD_DSI_LANE_COUNT 2
#define LCD_DSI_LANE_BITRATE_MBPS 500
#define LCD_DSI_PHY_PWR_LDO_CHAN 3
#define LCD_DSI_PHY_PWR_LDO_VOLTAGE_MV 2500
#define LCD_DPI_CLOCK_MHZ 30
#define LCD_DPI_BUFFER_COUNT 3
#define LCD_BACKLIGHT_TIMER LEDC_TIMER_1
#define LCD_BACKLIGHT_CHANNEL LEDC_CHANNEL_1
#define LCD_BACKLIGHT_DUTY_RES LEDC_TIMER_10_BIT
#define LCD_BACKLIGHT_DUTY_MAX ((1u << 10) - 1u)

typedef struct h2_waveshare_display_state {
    esp_lcd_dsi_bus_handle_t dsi_bus;
    esp_lcd_panel_io_handle_t panel_io;
    esp_lcd_panel_handle_t panel;
    esp_ldo_channel_handle_t phy_power;
    bool backlight_ready;
    bool opened;
} h2_waveshare_display_state_t;

static const char *TAG = "h2_waveshare_lcd";
static h2_waveshare_display_state_t s_display;

static const st7701_lcd_init_cmd_t s_panel_init[] = {
    {0xff, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xef, (uint8_t[]){0x08}, 1, 0},
    {0xff, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
    {0xc0, (uint8_t[]){0x63, 0x00}, 2, 0},
    {0xc1, (uint8_t[]){0x0d, 0x02}, 2, 0},
    {0xc2, (uint8_t[]){0x17, 0x08}, 2, 0},
    {0xcc, (uint8_t[]){0x10}, 1, 0},
    {0xb0, (uint8_t[]){0x40, 0xc9, 0x94, 0x0e, 0x10, 0x05, 0x0b, 0x09,
                       0x08, 0x26, 0x04, 0x52, 0x10, 0x69, 0x6b, 0x69}, 16, 0},
    {0xb1, (uint8_t[]){0x40, 0xd2, 0x98, 0x0c, 0x92, 0x07, 0x09, 0x08,
                       0x07, 0x25, 0x02, 0x0e, 0x0c, 0x6e, 0x78, 0x55}, 16, 0},
    {0xff, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},
    {0xb0, (uint8_t[]){0x5d}, 1, 0},
    {0xb1, (uint8_t[]){0x4e}, 1, 0},
    {0xb2, (uint8_t[]){0x87}, 1, 0},
    {0xb3, (uint8_t[]){0x80}, 1, 0},
    {0xb5, (uint8_t[]){0x4e}, 1, 0},
    {0xb7, (uint8_t[]){0x85}, 1, 0},
    {0xb8, (uint8_t[]){0x21}, 1, 0},
    {0xb9, (uint8_t[]){0x10, 0x1f}, 2, 0},
    {0xbb, (uint8_t[]){0x03}, 1, 0},
    {0xbc, (uint8_t[]){0x00}, 1, 0},
    {0xc1, (uint8_t[]){0x78}, 1, 0},
    {0xc2, (uint8_t[]){0x78}, 1, 0},
    {0xd0, (uint8_t[]){0x88}, 1, 0},
    {0xe0, (uint8_t[]){0x00, 0x3a, 0x02}, 3, 0},
    {0xe1, (uint8_t[]){0x04, 0xa0, 0x00, 0xa0, 0x05, 0xa0, 0x00, 0xa0,
                       0x00, 0x40, 0x40}, 11, 0},
    {0xe2, (uint8_t[]){0x30, 0x00, 0x40, 0x40, 0x32, 0xa0, 0x00, 0xa0,
                       0x00, 0xa0, 0x00, 0xa0, 0x00}, 13, 0},
    {0xe3, (uint8_t[]){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xe4, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xe5, (uint8_t[]){0x09, 0x2e, 0xa0, 0xa0, 0x0b, 0x30, 0xa0, 0xa0,
                       0x05, 0x2a, 0xa0, 0xa0, 0x07, 0x2c, 0xa0, 0xa0}, 16, 0},
    {0xe6, (uint8_t[]){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xe7, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xe8, (uint8_t[]){0x08, 0x2d, 0xa0, 0xa0, 0x0a, 0x2f, 0xa0, 0xa0,
                       0x04, 0x29, 0xa0, 0xa0, 0x06, 0x2b, 0xa0, 0xa0}, 16, 0},
    {0xeb, (uint8_t[]){0x00, 0x00, 0x4e, 0x4e, 0x00, 0x00, 0x00}, 7, 0},
    {0xec, (uint8_t[]){0x08, 0x01}, 2, 0},
    {0xed, (uint8_t[]){0xb0, 0x2b, 0x98, 0xa4, 0x56, 0x7f, 0xff, 0xff,
                       0xff, 0xff, 0xf7, 0x65, 0x4a, 0x89, 0xb2, 0x0b}, 16, 0},
    {0xef, (uint8_t[]){0x08, 0x08, 0x08, 0x45, 0x3f, 0x54}, 6, 0},
    {0xff, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},
    {0x11, NULL, 0, 120},
    {0x29, NULL, 0, 0},
};

static int map_error(esp_err_t err) {
    if (err == ESP_OK) {
        return H2_DISPLAY_OK;
    }
    if (err == ESP_ERR_NO_MEM) {
        return H2_DISPLAY_ERR_NO_MEMORY;
    }
    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_STATE) {
        return H2_DISPLAY_ERR_INVALID_ARG;
    }
    return H2_DISPLAY_ERR_IO;
}

static int set_brightness(h2_waveshare_display_state_t *state, uint32_t percent) {
    if (!state->backlight_ready) {
        return H2_DISPLAY_ERR_INVALID_STATE;
    }
    if (percent > 100u) {
        percent = 100u;
    }
    uint32_t duty = (percent * LCD_BACKLIGHT_DUTY_MAX) / 100u;
    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_BACKLIGHT_CHANNEL, duty);
    if (err == ESP_OK) {
        err = ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_BACKLIGHT_CHANNEL);
    }
    return map_error(err);
}

static void cleanup_display(h2_waveshare_display_state_t *state) {
    if (state->backlight_ready) {
        (void)set_brightness(state, 0u);
        (void)ledc_stop(LEDC_LOW_SPEED_MODE, LCD_BACKLIGHT_CHANNEL, 0u);
        state->backlight_ready = false;
    }
    if (state->panel != NULL) {
        (void)esp_lcd_panel_disp_on_off(state->panel, false);
        (void)esp_lcd_panel_del(state->panel);
        state->panel = NULL;
    }
    if (state->panel_io != NULL) {
        (void)esp_lcd_panel_io_del(state->panel_io);
        state->panel_io = NULL;
    }
    if (state->dsi_bus != NULL) {
        (void)esp_lcd_del_dsi_bus(state->dsi_bus);
        state->dsi_bus = NULL;
    }
    if (state->phy_power != NULL) {
        (void)esp_ldo_release_channel(state->phy_power);
        state->phy_power = NULL;
    }
    state->opened = false;
}

static int display_open(void *user) {
    h2_waveshare_display_state_t *state = user;
    if (state == NULL) {
        return H2_DISPLAY_ERR_INVALID_ARG;
    }
    if (state->opened) {
        return H2_DISPLAY_OK;
    }

    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LCD_BACKLIGHT_DUTY_RES,
        .timer_num = LCD_BACKLIGHT_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err == ESP_OK) {
        const ledc_channel_config_t channel = {
            .gpio_num = LCD_BACKLIGHT_GPIO,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LCD_BACKLIGHT_CHANNEL,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LCD_BACKLIGHT_TIMER,
            .duty = 0u,
            .hpoint = 0u,
            .flags = { .output_invert = 1u },
        };
        err = ledc_channel_config(&channel);
    }
    if (err != ESP_OK) {
        cleanup_display(state);
        return map_error(err);
    }
    state->backlight_ready = true;

    const esp_ldo_channel_config_t ldo = {
        .chan_id = LCD_DSI_PHY_PWR_LDO_CHAN,
        .voltage_mv = LCD_DSI_PHY_PWR_LDO_VOLTAGE_MV,
    };
    err = esp_ldo_acquire_channel(&ldo, &state->phy_power);
    if (err != ESP_OK) {
        cleanup_display(state);
        return map_error(err);
    }
    const esp_lcd_dsi_bus_config_t bus = {
        .bus_id = LCD_DSI_BUS_ID,
        .num_data_lanes = LCD_DSI_LANE_COUNT,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = LCD_DSI_LANE_BITRATE_MBPS,
    };
    err = esp_lcd_new_dsi_bus(&bus, &state->dsi_bus);
    if (err != ESP_OK) {
        cleanup_display(state);
        return map_error(err);
    }
    const esp_lcd_dbi_io_config_t dbi = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    err = esp_lcd_new_panel_io_dbi(state->dsi_bus, &dbi, &state->panel_io);
    if (err != ESP_OK) {
        cleanup_display(state);
        return map_error(err);
    }

    esp_lcd_dpi_panel_config_t dpi = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = LCD_DPI_CLOCK_MHZ,
        .virtual_channel = 0,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = LCD_DPI_BUFFER_COUNT,
        .video_timing = {
            .h_size = LCD_WIDTH,
            .v_size = LCD_HEIGHT,
            .hsync_back_porch = 42,
            .hsync_pulse_width = 12,
            .hsync_front_porch = 42,
            .vsync_back_porch = 2,
            .vsync_pulse_width = 8,
            .vsync_front_porch = 60,
        },
    };
    const st7701_vendor_config_t vendor = {
        .init_cmds = s_panel_init,
        .init_cmds_size = sizeof(s_panel_init) / sizeof(s_panel_init[0]),
        .mipi_config = {
            .dsi_bus = state->dsi_bus,
            .dpi_config = &dpi,
        },
        .flags = { .use_mipi_interface = 1u },
    };
    const esp_lcd_panel_dev_config_t panel = {
        .reset_gpio_num = LCD_RESET_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = (void *)&vendor,
    };
    err = esp_lcd_new_panel_st7701(state->panel_io, &panel, &state->panel);
    if (err == ESP_OK) {
        err = esp_lcd_panel_reset(state->panel);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_init(state->panel);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_disp_on_off(state->panel, true);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ST7701 open failed: %s", esp_err_to_name(err));
        cleanup_display(state);
        return map_error(err);
    }
    state->opened = true;
    int rc = set_brightness(state, 100u);
    if (rc != H2_DISPLAY_OK) {
        cleanup_display(state);
    }
    return rc;
}

static int display_get_info(void *user, h2_display_info_t *info) {
    h2_waveshare_display_state_t *state = user;
    if (state == NULL || info == NULL) {
        return H2_DISPLAY_ERR_INVALID_ARG;
    }
    if (!state->opened) {
        return H2_DISPLAY_ERR_INVALID_STATE;
    }
    *info = (h2_display_info_t){
        .width = LCD_WIDTH,
        .height = LCD_HEIGHT,
        .native_format = H2_DISPLAY_PIXEL_RGB565,
    };
    return H2_DISPLAY_OK;
}

static int display_draw_bitmap(
    void *user,
    const h2_display_rect_t *rect,
    const void *pixels,
    size_t stride_bytes,
    h2_display_pixel_format_t format) {
    h2_waveshare_display_state_t *state = user;
    if (state == NULL || rect == NULL || pixels == NULL ||
        rect->width <= 0 || rect->height <= 0) {
        return H2_DISPLAY_ERR_INVALID_ARG;
    }
    if (!state->opened) {
        return H2_DISPLAY_ERR_INVALID_STATE;
    }
    if (format != H2_DISPLAY_PIXEL_RGB565 ||
        stride_bytes < (size_t)rect->width * sizeof(uint16_t)) {
        return H2_DISPLAY_ERR_INVALID_ARG;
    }
    int64_t right = (int64_t)rect->x + (int64_t)rect->width;
    int64_t bottom = (int64_t)rect->y + (int64_t)rect->height;
    if (right <= 0 || bottom <= 0 ||
        rect->x >= LCD_WIDTH || rect->y >= LCD_HEIGHT) {
        return H2_DISPLAY_ERR_INVALID_ARG;
    }
    int x0 = rect->x < 0 ? 0 : rect->x;
    int y0 = rect->y < 0 ? 0 : rect->y;
    int x1 = right > LCD_WIDTH ? LCD_WIDTH : (int)right;
    int y1 = bottom > LCD_HEIGHT ? LCD_HEIGHT : (int)bottom;
    if (x0 >= x1 || y0 >= y1) {
        return H2_DISPLAY_ERR_INVALID_ARG;
    }
    const uint8_t *source = pixels;
    source += (size_t)((int64_t)y0 - (int64_t)rect->y) * stride_bytes;
    source += (size_t)((int64_t)x0 - (int64_t)rect->x) * sizeof(uint16_t);
    if (stride_bytes == (size_t)(x1 - x0) * sizeof(uint16_t)) {
        esp_err_t err = esp_lcd_panel_draw_bitmap(state->panel, x0, y0, x1, y1, source);
        if (err != ESP_OK) {
            return map_error(err);
        }
    } else {
        for (int y = y0; y < y1; ++y) {
            esp_err_t err = esp_lcd_panel_draw_bitmap(state->panel, x0, y, x1, y + 1, source);
            if (err != ESP_OK) {
                return map_error(err);
            }
            source += stride_bytes;
        }
    }
    return H2_DISPLAY_OK;
}

static int display_present(void *user) {
    h2_waveshare_display_state_t *state = user;
    return state != NULL && state->opened
        ? H2_DISPLAY_OK
        : H2_DISPLAY_ERR_INVALID_STATE;
}

static int display_set_brightness(void *user, uint32_t percent) {
    h2_waveshare_display_state_t *state = user;
    return state != NULL && state->opened
        ? set_brightness(state, percent)
        : H2_DISPLAY_ERR_INVALID_STATE;
}

static int display_close(void *user) {
    h2_waveshare_display_state_t *state = user;
    if (state == NULL) {
        return H2_DISPLAY_ERR_INVALID_ARG;
    }
    if (!state->opened && state->panel == NULL && state->dsi_bus == NULL) {
        return H2_DISPLAY_OK;
    }
    cleanup_display(state);
    return H2_DISPLAY_OK;
}

h2_pal_display_t *h2_esp_board_display(void) {
    static const h2_pal_display_vtable_t vtable = {
        .open = display_open,
        .get_info = display_get_info,
        .draw_bitmap = display_draw_bitmap,
        .present = display_present,
        .set_brightness_percent = display_set_brightness,
        .close = display_close,
    };
    static h2_pal_display_t display = {
        .user = &s_display,
        .vtable = &vtable,
    };
    return &display;
}

h2_pal_display_t *h2_esp_board_display_if_initialized(void) {
    return s_display.opened ? h2_esp_board_display() : NULL;
}
