#include "h2_bk7258_board_private.h"

#include "components/bk_display.h"
#include "components/media_types.h"
#include "driver/gpio.h"
#include "driver/lcd_types.h"
#include "driver/pwr_clk.h"
#include "frame_buffer.h"
#include "gpio_driver.h"
#include "lcd_panel_devices.h"
#include "media_service.h"
#include "modules/pm.h"
#include "os/os.h"

#include <components/log.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TAG "h2_bk_display"
#define LCD_LDO_PIN GPIO_13
#define LCD_BACKLIGHT_PIN GPIO_7
#define LCD_QSPI_RESET_PIN GPIO_40

extern void bk_psram_frame_buffer_init(void);

typedef enum h2_bk7258_display_bus {
    H2_BK7258_DISPLAY_BUS_RGB = 0,
    H2_BK7258_DISPLAY_BUS_QSPI = 1,
} h2_bk7258_display_bus_t;

#if defined(CONFIG_LCD_QSPI_ST77903_H0165Y008T) && CONFIG_LCD_QSPI_ST77903_H0165Y008T
#define H2_BK7258_HAS_QSPI_ST77903 1
#else
#define H2_BK7258_HAS_QSPI_ST77903 0
#endif

#define H2_BK7258_DISPLAY_BUS_DEFAULT H2_BK7258_DISPLAY_BUS_RGB

typedef struct h2_bk7258_display_state {
    bk_display_ctlr_handle_t handle;
    frame_buffer_t *shadow;
    uint32_t frame_size;
    uint16_t width;
    uint16_t height;
    h2_bk7258_display_bus_t bus;
    bool swap_rgb565_bytes;
    bool first_present_done;
    int initialized;
} h2_bk7258_display_state_t;

static h2_bk7258_display_state_t s_display_state = {
    .bus = H2_BK7258_DISPLAY_BUS_DEFAULT,
};

#if H2_BK7258_HAS_QSPI_ST77903
static bk_display_qspi_ctlr_config_t s_qspi_config = {
    .lcd_device = &lcd_device_st77903_h0165y008t,
    .qspi_id = 0,
    .reset_pin = LCD_QSPI_RESET_PIN,
    .te_pin = 0,
};
#endif

static bk_display_rgb_ctlr_config_t s_rgb_config = {
    .lcd_device = &lcd_device_h050iwv,
    .clk_pin = GPIO_0,
    .cs_pin = GPIO_12,
    .sda_pin = GPIO_1,
    .rst_pin = GPIO_6,
};

static uint16_t rgb888_to_rgb565(const uint8_t *pixel) {
    return (uint16_t)((((uint16_t)pixel[0] & 0xf8u) << 8) |
        (((uint16_t)pixel[1] & 0xfcu) << 3) |
        (((uint16_t)pixel[2]) >> 3));
}

static uint16_t rgb444_to_rgb565(uint16_t pixel) {
    uint16_t r = (uint16_t)((pixel >> 8) & 0x0fu);
    uint16_t g = (uint16_t)((pixel >> 4) & 0x0fu);
    uint16_t b = (uint16_t)(pixel & 0x0fu);
    return (uint16_t)((r << 12) | (r << 8) | (g << 7) | (g << 3) | (b << 1) | (b >> 3));
}

static uint16_t swap_rgb565(uint16_t pixel) {
    return (uint16_t)((pixel << 8) | (pixel >> 8));
}

static uint16_t encode_rgb565_for_bus(const h2_bk7258_display_state_t *state, uint16_t pixel) {
    return state->swap_rgb565_bytes ? swap_rgb565(pixel) : pixel;
}

static int avdk_result(avdk_err_t ret) {
    if (ret == AVDK_ERR_OK) {
        return H2_DISPLAY_OK;
    }
    if (ret == AVDK_ERR_NOMEM) {
        return H2_DISPLAY_ERR_NO_MEMORY;
    }
    if (ret == AVDK_ERR_INVAL) {
        return H2_DISPLAY_ERR_INVALID_ARG;
    }
    if (ret == AVDK_ERR_UNSUPPORTED) {
        return H2_DISPLAY_ERR_UNSUPPORTED;
    }
    return H2_DISPLAY_ERR_IO;
}

static void fill_frame_meta(frame_buffer_t *frame, uint16_t width, uint16_t height, uint32_t frame_size) {
    frame->fmt = PIXEL_FMT_RGB565;
    frame->width = width;
    frame->height = height;
    frame->length = frame_size;
    frame->size = frame_size;
}

static avdk_err_t display_frame_done(void *args) {
    frame_buffer_t *frame = (frame_buffer_t *)args;
    if (frame != NULL) {
        frame_buffer_display_free(frame);
    }
    return AVDK_ERR_OK;
}

static void lcd_backlight_open(uint8_t bl_io) {
    gpio_dev_unmap(bl_io);
    BK_LOG_ON_ERR(bk_gpio_enable_output(bl_io));
    BK_LOG_ON_ERR(bk_gpio_pull_up(bl_io));
    bk_gpio_set_output_high(bl_io);
}

static void lcd_backlight_close(uint8_t bl_io) {
    gpio_dev_unmap(bl_io);
    BK_LOG_ON_ERR(bk_gpio_enable_output(bl_io));
    BK_LOG_ON_ERR(bk_gpio_pull_down(bl_io));
    bk_gpio_set_output_low(bl_io);
}

static void deinit_display(h2_bk7258_display_state_t *state) {
    if (state == NULL || !state->initialized) {
        return;
    }

    if (state->handle != NULL) {
        (void)bk_display_close(state->handle);
    }
    lcd_backlight_close(LCD_BACKLIGHT_PIN);
    if (state->shadow != NULL) {
        frame_buffer_display_free(state->shadow);
        state->shadow = NULL;
    }
    if (state->handle != NULL) {
        (void)bk_display_delete(state->handle);
        state->handle = NULL;
    }

    state->frame_size = 0u;
    state->width = 0u;
    state->height = 0u;
    state->swap_rgb565_bytes = false;
    state->first_present_done = false;
    state->initialized = 0;
}

static int init_display(h2_bk7258_display_state_t *state) {
    if (state->initialized) {
        return H2_DISPLAY_OK;
    }

    avdk_err_t ret = AVDK_ERR_OK;

#if H2_BK7258_HAS_QSPI_ST77903
    if (state->bus == H2_BK7258_DISPLAY_BUS_QSPI) {
        (void)media_service_init();
        bk_psram_frame_buffer_init();
        ret = bk_display_qspi_new(&state->handle, &s_qspi_config);
        if (ret != AVDK_ERR_OK) {
            BK_LOGE(TAG, "bk_display_qspi_new failed: %d\r\n", ret);
            return avdk_result(ret);
        }
        state->width = s_qspi_config.lcd_device->width;
        state->height = s_qspi_config.lcd_device->height;
        state->swap_rgb565_bytes = true;
    } else
#endif
    {
        (void)media_service_init();
        (void)bk_pm_module_vote_psram_ctrl(PM_POWER_PSRAM_MODULE_NAME_LVGL_CODE_RUN, PM_POWER_MODULE_STATE_ON);

        ret = bk_display_rgb_new(&state->handle, &s_rgb_config);
        if (ret != AVDK_ERR_OK) {
            BK_LOGE(TAG, "bk_display_rgb_new failed: %d\r\n", ret);
            return avdk_result(ret);
        }
        state->width = s_rgb_config.lcd_device->width;
        state->height = s_rgb_config.lcd_device->height;
        state->swap_rgb565_bytes = false;
    }

    state->frame_size = (uint32_t)state->width * (uint32_t)state->height * sizeof(uint16_t);
    state->shadow = frame_buffer_display_malloc(state->frame_size);
    if (state->shadow == NULL) {
        BK_LOGE(TAG, "frame_buffer_display_malloc failed\r\n");
        (void)bk_display_delete(state->handle);
        state->handle = NULL;
        return H2_DISPLAY_ERR_NO_MEMORY;
    }
    fill_frame_meta(state->shadow, state->width, state->height, state->frame_size);
    os_memset(state->shadow->frame, 0, state->frame_size);

    bk_pm_module_vote_ctrl_external_ldo(GPIO_CTRL_LDO_MODULE_LCD, LCD_LDO_PIN, GPIO_OUTPUT_STATE_HIGH);
    ret = bk_display_open(state->handle);
    if (ret != AVDK_ERR_OK) {
        BK_LOGE(TAG, "bk_display_open failed: %d\r\n", ret);
        frame_buffer_display_free(state->shadow);
        state->shadow = NULL;
        (void)bk_display_delete(state->handle);
        state->handle = NULL;
        return avdk_result(ret);
    }
    lcd_backlight_open(LCD_BACKLIGHT_PIN);

    state->first_present_done = false;
    state->initialized = 1;
    return H2_DISPLAY_OK;
}

static int clip_rect(
    const h2_bk7258_display_state_t *state,
    const h2_display_rect_t *rect,
    h2_display_rect_t *clipped) {
    int x1 = rect->x;
    int y1 = rect->y;
    int x2 = rect->x + rect->width;
    int y2 = rect->y + rect->height;
    int width = state->width;
    int height = state->height;

    if (x1 < 0) {
        x1 = 0;
    }
    if (y1 < 0) {
        y1 = 0;
    }
    if (x2 > width) {
        x2 = width;
    }
    if (y2 > height) {
        y2 = height;
    }
    if (x1 >= x2 || y1 >= y2) {
        return H2_DISPLAY_ERR_INVALID_ARG;
    }

    clipped->x = x1;
    clipped->y = y1;
    clipped->width = x2 - x1;
    clipped->height = y2 - y1;
    return H2_DISPLAY_OK;
}

static int bk_get_info(void *user, h2_display_info_t *info) {
    h2_bk7258_display_state_t *state = (h2_bk7258_display_state_t *)user;
    if (!state->initialized) {
        return H2_DISPLAY_ERR_INVALID_STATE;
    }

    info->width = state->width;
    info->height = state->height;
    info->native_format = H2_DISPLAY_PIXEL_RGB565;
    return H2_DISPLAY_OK;
}

static int bk_draw_bitmap(
    void *user,
    const h2_display_rect_t *rect,
    const void *pixels,
    size_t stride_bytes,
    h2_display_pixel_format_t format) {
    h2_bk7258_display_state_t *state = (h2_bk7258_display_state_t *)user;
    if (!state->initialized) {
        return H2_DISPLAY_ERR_INVALID_STATE;
    }

    size_t src_pixel_size = 0;
    if (format == H2_DISPLAY_PIXEL_RGB565 || format == H2_DISPLAY_PIXEL_RGB444) {
        src_pixel_size = 2u;
    } else if (format == H2_DISPLAY_PIXEL_RGB888) {
        src_pixel_size = 3u;
    } else {
        return H2_DISPLAY_ERR_UNSUPPORTED;
    }
    if (stride_bytes < (size_t)rect->width * src_pixel_size) {
        return H2_DISPLAY_ERR_INVALID_ARG;
    }

    h2_display_rect_t clipped;
    int rc = clip_rect(state, rect, &clipped);
    if (rc != H2_DISPLAY_OK) {
        return rc;
    }

    const uint8_t *src = (const uint8_t *)pixels;
    src += (size_t)(clipped.y - rect->y) * stride_bytes;
    src += (size_t)(clipped.x - rect->x) * src_pixel_size;

    for (int row = 0; row < clipped.height; ++row) {
        uint16_t *dst = (uint16_t *)state->shadow->frame +
            ((size_t)(clipped.y + row) * (size_t)state->shadow->width) +
            (size_t)clipped.x;
        const uint8_t *src_row = src + (size_t)row * stride_bytes;
        if (format == H2_DISPLAY_PIXEL_RGB565) {
            if (!state->swap_rgb565_bytes) {
                memcpy(dst, src_row, (size_t)clipped.width * sizeof(uint16_t));
            } else {
                const uint16_t *src16 = (const uint16_t *)src_row;
                for (int col = 0; col < clipped.width; ++col) {
                    dst[col] = encode_rgb565_for_bus(state, src16[col]);
                }
            }
        } else if (format == H2_DISPLAY_PIXEL_RGB888) {
            for (int col = 0; col < clipped.width; ++col) {
                dst[col] = encode_rgb565_for_bus(state, rgb888_to_rgb565(src_row + (size_t)col * 3u));
            }
        } else {
            const uint16_t *src16 = (const uint16_t *)src_row;
            for (int col = 0; col < clipped.width; ++col) {
                dst[col] = encode_rgb565_for_bus(state, rgb444_to_rgb565(src16[col]));
            }
        }
    }

    return H2_DISPLAY_OK;
}

static int flush_shadow_once(h2_bk7258_display_state_t *state) {
    frame_buffer_t *display_frame = frame_buffer_display_malloc(state->frame_size);
    if (display_frame == NULL) {
        BK_LOGE(TAG, "display frame malloc failed\r\n");
        return H2_DISPLAY_ERR_NO_MEMORY;
    }
    os_memcpy(display_frame->frame, state->shadow->frame, state->frame_size);
    fill_frame_meta(display_frame, state->width, state->height, state->frame_size);

    avdk_err_t ret = bk_display_flush(state->handle, display_frame, display_frame_done);
    if (ret != AVDK_ERR_OK) {
        frame_buffer_display_free(display_frame);
    }
    return avdk_result(ret);
}

static int bk_present(void *user) {
    h2_bk7258_display_state_t *state = (h2_bk7258_display_state_t *)user;
    if (!state->initialized) {
        return H2_DISPLAY_ERR_INVALID_STATE;
    }

    uint32_t flush_count = state->first_present_done ? 1u : 3u;
    int rc = H2_DISPLAY_OK;
    for (uint32_t i = 0; i < flush_count; ++i) {
        rc = flush_shadow_once(state);
        if (rc != H2_DISPLAY_OK) {
            return rc;
        }
        if (!state->first_present_done) {
            rtos_delay_milliseconds(100);
        }
    }
    state->first_present_done = true;
    return rc;
}

static int bk_set_brightness_percent(void *user, uint32_t percent) {
    h2_bk7258_display_state_t *state = (h2_bk7258_display_state_t *)user;
    if (!state->initialized) {
        return H2_DISPLAY_ERR_INVALID_STATE;
    }
    if (percent == 0u) {
        lcd_backlight_close(LCD_BACKLIGHT_PIN);
    } else {
        lcd_backlight_open(LCD_BACKLIGHT_PIN);
    }
    return H2_DISPLAY_OK;
}

static int bk_open(void *user) {
    h2_bk7258_display_state_t *state = (h2_bk7258_display_state_t *)user;
    return init_display(state);
}

static int bk_close(void *user) {
    h2_bk7258_display_state_t *state = (h2_bk7258_display_state_t *)user;
    deinit_display(state);
    return H2_DISPLAY_OK;
}

int h2_bk7258_board_display_black(void) {
    h2_pal_display_t *display = h2_bk7258_board_display();
    int rc = h2_pal_display_open(display);
    if (rc != H2_DISPLAY_OK) {
        return rc;
    }

    h2_bk7258_display_state_t *state = (h2_bk7258_display_state_t *)display->user;
    os_memset(state->shadow->frame, 0, state->frame_size);
    return bk_present(display);
}

h2_pal_display_t *h2_bk7258_board_display(void) {
    static const h2_pal_display_vtable_t vtable = {
        .open = bk_open,
        .get_info = bk_get_info,
        .draw_bitmap = bk_draw_bitmap,
        .present = bk_present,
        .set_brightness_percent = bk_set_brightness_percent,
        .close = bk_close,
    };
    static h2_pal_display_t display = {
        .user = &s_display_state,
        .vtable = &vtable,
    };
    return &display;
}
