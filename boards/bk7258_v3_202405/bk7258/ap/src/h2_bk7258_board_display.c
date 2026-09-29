#include "h2_bk7258_board_private.h"

#include "components/bk_display.h"
#include "components/media_types.h"
#include "driver/gpio.h"
#include "driver/lcd_types.h"
#include "driver/lcd.h"
#include "lcd_disp_ll_macro_def.h"
#include "driver/pwr_clk.h"
#include "driver/pwm.h"
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
    bool backlight_pwm_initialized;
    int initialized;
} h2_bk7258_display_state_t;

/* Media slab and shared media timer are process-owned, initialized once. */
static bool s_media_initialized;
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
    /* H050IWV has no SPI init callback. Disable that optional control bus;
     * GPIO0/1 belong to H2Loader UART1 and must remain mapped to UART. */
    .clk_pin = GPIO_NUM,
    .cs_pin = GPIO_NUM,
    .sda_pin = GPIO_NUM,
    .rst_pin = GPIO_NUM,
};

/* The SDK leaves RGB GPIO init disabled; H2Loader's UART-only boot map
 * must not determine whether this board Display sends physical panel signals. */
static int init_rgb_pins(void) {
    static const struct { gpio_id_t pin; gpio_dev_t function; } pins[] = {
        {GPIO_14, GPIO_DEV_LCD_CLK},
        {GPIO_15, GPIO_DEV_LCD_DISP},
        {GPIO_16, GPIO_DEV_LCD_DE},
        {GPIO_17, GPIO_DEV_LCD_HSYNC},
        {GPIO_18, GPIO_DEV_LCD_VSYNC},
        {GPIO_19, GPIO_DEV_LCD_R7},
        {GPIO_20, GPIO_DEV_LCD_R6},
        {GPIO_21, GPIO_DEV_LCD_R5},
        {GPIO_22, GPIO_DEV_LCD_R4},
        {GPIO_23, GPIO_DEV_LCD_R3},
        {GPIO_24, GPIO_DEV_LCD_G7},
        {GPIO_25, GPIO_DEV_LCD_G6},
        {GPIO_26, GPIO_DEV_LCD_G5},
        {GPIO_40, GPIO_DEV_LCD_G4},
        {GPIO_41, GPIO_DEV_LCD_G3},
        {GPIO_42, GPIO_DEV_LCD_G2},
        {GPIO_43, GPIO_DEV_LCD_B7},
        {GPIO_44, GPIO_DEV_LCD_B6},
        {GPIO_45, GPIO_DEV_LCD_B5},
        {GPIO_46, GPIO_DEV_LCD_B4},
        {GPIO_47, GPIO_DEV_LCD_B3},
        {GPIO_48, GPIO_DEV_LCD_R2},
        {GPIO_49, GPIO_DEV_LCD_R1},
        {GPIO_50, GPIO_DEV_LCD_R0},
        {GPIO_51, GPIO_DEV_LCD_G1},
        {GPIO_52, GPIO_DEV_LCD_G0},
        {GPIO_53, GPIO_DEV_LCD_B2},
        {GPIO_54, GPIO_DEV_LCD_B1},
        {GPIO_55, GPIO_DEV_LCD_B0},
    };
    for (size_t i = 0; i < sizeof(pins)/sizeof(pins[0]); ++i) {
        gpio_dev_unmap(pins[i].pin);
        if (gpio_dev_map(pins[i].pin, pins[i].function) != BK_OK ||
            !bk_gpio_set_capacity(pins[i].pin, GPIO_DRIVER_CAPACITY_3))
            return H2_DISPLAY_ERR_IO;
    }
    printf("H2_BK_DISPLAY_RGB_PINS mapped=%u uart1_preserved=1\n",
        (unsigned)(sizeof(pins)/sizeof(pins[0])));
    return H2_DISPLAY_OK;
}

static uint16_t rgb888_to_rgb565(const uint8_t *pixel) {
    return (uint16_t)((((uint16_t)pixel[0] & 0xf8u) << 8) |
        (((uint16_t)pixel[1] & 0xfcu) << 3) |
        (((uint16_t)pixel[2]) >> 3));
}

static uint16_t rgb444_to_rgb565(uint16_t pixel) {
    uint16_t r = (uint16_t)((pixel >> 8) & 0x0fu);
    uint16_t g = (uint16_t)((pixel >> 4) & 0x0fu);
    uint16_t b = (uint16_t)(pixel & 0x0fu);
    return (uint16_t)((((r << 1) | (r >> 3)) << 11) | (((g << 2) | (g >> 2)) << 5) | (b << 1) | (b >> 3));
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
    if (state->backlight_pwm_initialized) {
        (void)bk_pwm_deinit(PWM_CH_1);
        state->backlight_pwm_initialized = false;
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
    if (!s_media_initialized) {
        if (media_service_init() != BK_OK ||
            bk_pm_module_vote_psram_ctrl(PM_POWER_PSRAM_MODULE_NAME_LVGL_CODE_RUN,
                PM_POWER_MODULE_STATE_ON) != BK_OK)
            return H2_DISPLAY_ERR_IO;
        bk_psram_frame_buffer_init();
        s_media_initialized = true;
    }

#if H2_BK7258_HAS_QSPI_ST77903
    if (state->bus == H2_BK7258_DISPLAY_BUS_QSPI) {
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
        int pin_rc = init_rgb_pins();
        if (pin_rc != H2_DISPLAY_OK) return pin_rc;
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
    int64_t x1 = rect->x;
    int64_t y1 = rect->y;
    int64_t x2 = x1 + rect->width;
    int64_t y2 = y1 + rect->height;
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
    const size_t row_bytes = (size_t)rect->width * src_pixel_size;
    if (rect->width <= 0 || rect->height <= 0 ||
        (size_t)rect->width > SIZE_MAX / src_pixel_size ||
        stride_bytes < row_bytes ||
        ((size_t)rect->height - 1u) > (SIZE_MAX - row_bytes) / stride_bytes) {
        return H2_DISPLAY_ERR_INVALID_ARG;
    }

    h2_display_rect_t clipped;
    int rc = clip_rect(state, rect, &clipped);
    if (rc != H2_DISPLAY_OK) {
        return rc;
    }

    const uint8_t *src = (const uint8_t *)pixels;
    src += (size_t)((int64_t)clipped.y - rect->y) * stride_bytes;
    src += (size_t)((int64_t)clipped.x - rect->x) * src_pixel_size;

    for (int row = 0; row < clipped.height; ++row) {
        uint16_t *dst = (uint16_t *)state->shadow->frame +
            ((size_t)(clipped.y + row) * (size_t)state->shadow->width) +
            (size_t)clipped.x;
        const uint8_t *src_row = src + (size_t)row * stride_bytes;
        if (format == H2_DISPLAY_PIXEL_RGB565) {
            if (!state->swap_rgb565_bytes) {
                memcpy(dst, src_row, (size_t)clipped.width * sizeof(uint16_t));
            } else {
                for (int col = 0; col < clipped.width; ++col) {
                    uint16_t pixel;
                    memcpy(&pixel, src_row + (size_t)col * 2u, 2u);
                    dst[col] = encode_rgb565_for_bus(state, pixel);
                }
            }
        } else if (format == H2_DISPLAY_PIXEL_RGB888) {
            for (int col = 0; col < clipped.width; ++col) {
                dst[col] = encode_rgb565_for_bus(state, rgb888_to_rgb565(src_row + (size_t)col * 3u));
            }
        } else {
            for (int col = 0; col < clipped.width; ++col) {
                uint16_t pixel;
                memcpy(&pixel, src_row + (size_t)col * 2u, 2u);
                dst[col] = encode_rgb565_for_bus(state, rgb444_to_rgb565(pixel));
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
    if (percent > 100u) return H2_DISPLAY_ERR_INVALID_ARG;
    if (percent == 0u || percent == 100u) {
        if (state->backlight_pwm_initialized) {
            if (bk_pwm_deinit(PWM_CH_1) != BK_OK) return H2_DISPLAY_ERR_IO;
            state->backlight_pwm_initialized = false;
        }
        if (percent == 0u) lcd_backlight_close(LCD_BACKLIGHT_PIN);
        else lcd_backlight_open(LCD_BACKLIGHT_PIN);
    } else {
        /* 26MHz clock / 26000 = 1kHz PWM. GPIO map is board-owned and
         * excludes SDK's default GPIO19, which carries LCD red pixels. */
        const uint32_t period = 26000u;
        if (!state->backlight_pwm_initialized) {
            const pwm_init_config_t config = {
                .period_cycle = period, .duty_cycle = period * percent / 100u};
            if (bk_pwm_driver_init() != BK_OK ||
                bk_pwm_init(PWM_CH_1, &config) != BK_OK) return H2_DISPLAY_ERR_IO;
            state->backlight_pwm_initialized = true;
            if (bk_pwm_start(PWM_CH_1) != BK_OK) return H2_DISPLAY_ERR_IO;
        } else {
            pwm_period_duty_config_t config = {
                .period_cycle = period, .duty_cycle = period * percent / 100u};
            if (bk_pwm_set_period_duty(PWM_CH_1, &config) != BK_OK)
                return H2_DISPLAY_ERR_IO;
        }
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
    return h2_pal_display_present(display);
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

int h2_bk7258_board_display_capture(uint16_t *pixels, size_t capacity) {
    h2_bk7258_display_state_t *state = &s_display_state;
    if (!state->initialized) return H2_DISPLAY_ERR_INVALID_STATE;
    if (!pixels || capacity < (size_t)state->width*state->height)
        return H2_DISPLAY_ERR_INVALID_ARG;
    if (state->bus != H2_BK7258_DISPLAY_BUS_RGB)
        return H2_DISPLAY_ERR_UNSUPPORTED;
    /* Driver queue and one full refresh must complete before taking this
     * diagnostic copy. No Display calls run concurrently with this capture. */
    rtos_delay_milliseconds(150);
    uint32_t before = lcd_disp_ll_get_disp_status_rgb_ver_cnt();
    bool advancing = false;
    for (unsigned i=0; i<20; ++i) {
        rtos_delay_milliseconds(1);
        if (lcd_disp_ll_get_disp_status_rgb_ver_cnt() != before) { advancing = true; break; }
    }
    uintptr_t source = lcd_disp_ll_get_mater_rd_base_addr();
    if (!advancing || source == 0u) return H2_DISPLAY_ERR_IO;
    memcpy(pixels, (const void *)source, state->frame_size);
    return H2_DISPLAY_OK;
}
