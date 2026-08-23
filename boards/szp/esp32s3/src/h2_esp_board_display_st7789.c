#include "h2_esp_board_private.h"

#include "h2_esp_szp_board_internal.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define LCD_HOST SPI3_HOST
#define LCD_WIDTH 320
#define LCD_HEIGHT 240
#define LCD_CS_GPIO GPIO_NUM_NC
#define LCD_SCLK_GPIO GPIO_NUM_41
#define LCD_MOSI_GPIO GPIO_NUM_40
#define LCD_DC_GPIO GPIO_NUM_39
#define LCD_RST_GPIO GPIO_NUM_NC
#define LCD_BL_GPIO GPIO_NUM_42
#define LCD_PIXEL_CLK_HZ (80 * 1000 * 1000)
#define LCD_CMD_BITS 8
#define LCD_PARAM_BITS 8
#define LCD_BITS_PER_PIXEL 16
#define LCD_DMA_BUFFER_ROWS 10
#define LCD_DMA_BUFFER_PIXELS (LCD_WIDTH * LCD_DMA_BUFFER_ROWS)
#define LCD_DMA_BUFFER_BYTES (LCD_DMA_BUFFER_PIXELS * sizeof(uint16_t))
#define LCD_CMD_SWRESET 0x01
#define LCD_CMD_SLPOUT 0x11
#define LCD_CMD_INVON 0x21
#define LCD_CMD_DISPOFF 0x28
#define LCD_CMD_DISPON 0x29
#define LCD_CMD_CASET 0x2A
#define LCD_CMD_RASET 0x2B
#define LCD_CMD_RAMWR 0x2C
#define LCD_CMD_MADCTL 0x36
#define LCD_CMD_COLMOD 0x3A
#define LCD_CMD_RAMCTRL 0xB0
#define LCD_MADCTL_MX 0x40
#define LCD_MADCTL_MV 0x20
#define LCD_BACKLIGHT_TIMER LEDC_TIMER_0
#define LCD_BACKLIGHT_CHANNEL LEDC_CHANNEL_0
#define LCD_BACKLIGHT_DUTY_RES LEDC_TIMER_10_BIT
#define LCD_BACKLIGHT_DUTY_MAX ((1u << 10) - 1u)

typedef struct h2_esp_szp_display_state {
    esp_lcd_panel_io_handle_t panel_io;
    uint16_t *dma_buffer;
    size_t dma_buffer_pixels;
    bool initialized;
    bool opened;
} h2_esp_szp_display_state_t;

static const char *TAG = "h2_esp_szp";
static h2_esp_szp_display_state_t s_display_state;

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

static uint16_t rgb565_to_panel(uint16_t pixel) {
    return (uint16_t)((pixel << 8) | (pixel >> 8));
}

static int esp_result(esp_err_t err) {
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

static int drain_panel_io(h2_esp_szp_display_state_t *state) {
    esp_err_t err = esp_lcd_panel_io_tx_param(state->panel_io, -1, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel io drain failed: %s", esp_err_to_name(err));
    }
    return esp_result(err);
}

static void delay_ms(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static int st7789_cmd(h2_esp_szp_display_state_t *state, int cmd, const void *data, size_t len) {
    esp_err_t err = esp_lcd_panel_io_tx_param(state->panel_io, cmd, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "st7789 cmd=0x%02x failed: %s", cmd, esp_err_to_name(err));
    }
    return esp_result(err);
}

static int st7789_color(h2_esp_szp_display_state_t *state, int cmd, const void *data, size_t len) {
    esp_err_t err = esp_lcd_panel_io_tx_color(state->panel_io, cmd, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "st7789 color cmd=0x%02x failed: %s", cmd, esp_err_to_name(err));
        return esp_result(err);
    }
    return drain_panel_io(state);
}

static int st7789_set_window(h2_esp_szp_display_state_t *state, int x, int y, int width, int height) {
    const uint16_t x0 = (uint16_t)x;
    const uint16_t y0 = (uint16_t)y;
    const uint16_t x1 = (uint16_t)(x + width - 1);
    const uint16_t y1 = (uint16_t)(y + height - 1);
    const uint8_t column[] = {
        (uint8_t)(x0 >> 8),
        (uint8_t)(x0 & 0xffu),
        (uint8_t)(x1 >> 8),
        (uint8_t)(x1 & 0xffu),
    };
    const uint8_t row[] = {
        (uint8_t)(y0 >> 8),
        (uint8_t)(y0 & 0xffu),
        (uint8_t)(y1 >> 8),
        (uint8_t)(y1 & 0xffu),
    };

    int rc = st7789_cmd(state, LCD_CMD_CASET, column, sizeof(column));
    if (rc != H2_DISPLAY_OK) {
        return rc;
    }
    return st7789_cmd(state, LCD_CMD_RASET, row, sizeof(row));
}

static int st7789_open(h2_esp_szp_display_state_t *state) {
    int rc = h2_esp_szp_board_set_lcd_cs(1);
    if (rc != 0) {
        return H2_DISPLAY_ERR_IO;
    }
    rc = st7789_cmd(state, LCD_CMD_SWRESET, NULL, 0);
    if (rc != H2_DISPLAY_OK) {
        return rc;
    }
    delay_ms(20);

    rc = h2_esp_szp_board_set_lcd_cs(0);
    if (rc != 0) {
        return H2_DISPLAY_ERR_IO;
    }

    rc = st7789_cmd(state, LCD_CMD_SLPOUT, NULL, 0);
    if (rc != H2_DISPLAY_OK) {
        return rc;
    }
    delay_ms(100);

    uint8_t madctl = 0x00;
    rc = st7789_cmd(state, LCD_CMD_MADCTL, &madctl, sizeof(madctl));
    if (rc != H2_DISPLAY_OK) {
        return rc;
    }
    const uint8_t colmod = 0x55;
    rc = st7789_cmd(state, LCD_CMD_COLMOD, &colmod, sizeof(colmod));
    if (rc != H2_DISPLAY_OK) {
        return rc;
    }
    const uint8_t ramctrl[] = { 0x00, 0xF0 };
    rc = st7789_cmd(state, LCD_CMD_RAMCTRL, ramctrl, sizeof(ramctrl));
    if (rc != H2_DISPLAY_OK) {
        return rc;
    }
    rc = st7789_cmd(state, LCD_CMD_INVON, NULL, 0);
    if (rc != H2_DISPLAY_OK) {
        return rc;
    }
    madctl = LCD_MADCTL_MV;
    rc = st7789_cmd(state, LCD_CMD_MADCTL, &madctl, sizeof(madctl));
    if (rc != H2_DISPLAY_OK) {
        return rc;
    }
    madctl = LCD_MADCTL_MX | LCD_MADCTL_MV;
    rc = st7789_cmd(state, LCD_CMD_MADCTL, &madctl, sizeof(madctl));
    if (rc != H2_DISPLAY_OK) {
        return rc;
    }
    return st7789_cmd(state, LCD_CMD_DISPON, NULL, 0);
}

static int set_backlight_percent(uint32_t percent) {
    if (percent > 100u) {
        percent = 100u;
    }
    const uint32_t duty = (percent * LCD_BACKLIGHT_DUTY_MAX) / 100u;
    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_BACKLIGHT_CHANNEL, duty);
    if (err == ESP_OK) {
        err = ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_BACKLIGHT_CHANNEL);
    }
    return esp_result(err);
}

static int init_backlight(void) {
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LCD_BACKLIGHT_DUTY_RES,
        .timer_num = LCD_BACKLIGHT_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) {
        return esp_result(err);
    }
    const ledc_channel_config_t channel = {
        .gpio_num = LCD_BL_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LCD_BACKLIGHT_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LCD_BACKLIGHT_TIMER,
        .duty = 0,
        .hpoint = 0,
        .flags = {
            .output_invert = 1,
        },
    };
    err = ledc_channel_config(&channel);
    if (err != ESP_OK) {
        return esp_result(err);
    }
    return set_backlight_percent(0);
}

static int alloc_dma_buffer(h2_esp_szp_display_state_t *state) {
    if (state->dma_buffer != NULL) {
        return H2_DISPLAY_OK;
    }

    state->dma_buffer = (uint16_t *)heap_caps_aligned_alloc(
        64,
        LCD_DMA_BUFFER_BYTES,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (state->dma_buffer == NULL) {
        ESP_LOGE(TAG, "display dma buffer alloc failed bytes=%u", (unsigned)LCD_DMA_BUFFER_BYTES);
        return H2_DISPLAY_ERR_NO_MEMORY;
    }
    state->dma_buffer_pixels = LCD_DMA_BUFFER_PIXELS;
    ESP_LOGI(TAG, "display dma buffer bytes=%u rows=%u", (unsigned)LCD_DMA_BUFFER_BYTES, (unsigned)LCD_DMA_BUFFER_ROWS);
    return H2_DISPLAY_OK;
}

static int init_display(h2_esp_szp_display_state_t *state) {
    if (state->initialized) {
        return H2_DISPLAY_OK;
    }

    ESP_LOGI(TAG, "init SZP ST7789 SPI panel");
    int rc = h2_esp_szp_board_init_io();
    if (rc != 0) {
        return H2_DISPLAY_ERR_IO;
    }
    rc = init_backlight();
    if (rc != H2_DISPLAY_OK) {
        return rc;
    }

    const spi_bus_config_t buscfg = {
        .sclk_io_num = LCD_SCLK_GPIO,
        .mosi_io_num = LCD_MOSI_GPIO,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_DMA_BUFFER_BYTES,
    };
    esp_err_t err = spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return esp_result(err);
    }

    rc = alloc_dma_buffer(state);
    if (rc != H2_DISPLAY_OK) {
        return rc;
    }

    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_DC_GPIO,
        .cs_gpio_num = LCD_CS_GPIO,
        .pclk_hz = LCD_PIXEL_CLK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 2,
        .trans_queue_depth = 10,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &state->panel_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi failed: %s", esp_err_to_name(err));
        return esp_result(err);
    }

    rc = st7789_open(state);
    if (rc != H2_DISPLAY_OK) {
        ESP_LOGE(TAG, "panel init failed rc=%d", rc);
        return rc;
    }

    rc = set_backlight_percent(100u);
    if (rc != H2_DISPLAY_OK) {
        return rc;
    }

    state->initialized = true;
    return H2_DISPLAY_OK;
}

static int clip_rect(const h2_display_rect_t *rect, h2_display_rect_t *clipped) {
    if (rect == NULL || clipped == NULL || rect->width <= 0 || rect->height <= 0) {
        return H2_DISPLAY_ERR_INVALID_ARG;
    }

    int x1 = rect->x;
    int y1 = rect->y;
    int x2 = rect->x + rect->width;
    int y2 = rect->y + rect->height;

    if (x1 < 0) {
        x1 = 0;
    }
    if (y1 < 0) {
        y1 = 0;
    }
    if (x2 > LCD_WIDTH) {
        x2 = LCD_WIDTH;
    }
    if (y2 > LCD_HEIGHT) {
        y2 = LCD_HEIGHT;
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

static int source_pixel_size(h2_display_pixel_format_t format, size_t *pixel_size) {
    if (pixel_size == NULL) {
        return H2_DISPLAY_ERR_INVALID_ARG;
    }
    if (format == H2_DISPLAY_PIXEL_RGB565 || format == H2_DISPLAY_PIXEL_RGB444) {
        *pixel_size = 2u;
        return H2_DISPLAY_OK;
    }
    if (format == H2_DISPLAY_PIXEL_RGB888) {
        *pixel_size = 3u;
        return H2_DISPLAY_OK;
    }
    return H2_DISPLAY_ERR_UNSUPPORTED;
}

static int validate_source_bitmap(
    const h2_display_rect_t *src_rect,
    const void *pixels,
    size_t stride_bytes,
    h2_display_pixel_format_t format,
    size_t *src_pixel_size) {
    if (src_rect == NULL || pixels == NULL || src_pixel_size == NULL) {
        return H2_DISPLAY_ERR_INVALID_ARG;
    }
    int rc = source_pixel_size(format, src_pixel_size);
    if (rc != H2_DISPLAY_OK) {
        return rc;
    }
    if (stride_bytes < (size_t)src_rect->width * *src_pixel_size) {
        return H2_DISPLAY_ERR_INVALID_ARG;
    }
    return H2_DISPLAY_OK;
}

static void convert_bitmap_to_rgb565(
    const h2_display_rect_t *src_rect,
    const h2_display_rect_t *clipped,
    const void *pixels,
    size_t stride_bytes,
    h2_display_pixel_format_t format,
    size_t src_pixel_size,
    uint16_t *out) {
    const uint8_t *src = (const uint8_t *)pixels;
    src += (size_t)(clipped->y - src_rect->y) * stride_bytes;
    src += (size_t)(clipped->x - src_rect->x) * src_pixel_size;

    for (int row = 0; row < clipped->height; ++row) {
        uint16_t *dst = out + (size_t)row * (size_t)clipped->width;
        const uint8_t *src_row = src + (size_t)row * stride_bytes;
        if (format == H2_DISPLAY_PIXEL_RGB565) {
            const uint16_t *src16 = (const uint16_t *)src_row;
            for (int col = 0; col < clipped->width; ++col) {
                dst[col] = rgb565_to_panel(src16[col]);
            }
        } else if (format == H2_DISPLAY_PIXEL_RGB888) {
            for (int col = 0; col < clipped->width; ++col) {
                dst[col] = rgb565_to_panel(rgb888_to_rgb565(src_row + (size_t)col * 3u));
            }
        } else {
            const uint16_t *src16 = (const uint16_t *)src_row;
            for (int col = 0; col < clipped->width; ++col) {
                dst[col] = rgb565_to_panel(rgb444_to_rgb565(src16[col]));
            }
        }
    }
}

static int szp_get_info(void *user, h2_display_info_t *info) {
    h2_esp_szp_display_state_t *state = (h2_esp_szp_display_state_t *)user;
    if (!state->opened) {
        return H2_DISPLAY_ERR_INVALID_STATE;
    }

    info->width = LCD_WIDTH;
    info->height = LCD_HEIGHT;
    info->native_format = H2_DISPLAY_PIXEL_RGB565;
    return H2_DISPLAY_OK;
}

static int szp_draw_bitmap(
    void *user,
    const h2_display_rect_t *rect,
    const void *pixels,
    size_t stride_bytes,
    h2_display_pixel_format_t format) {
    h2_esp_szp_display_state_t *state = (h2_esp_szp_display_state_t *)user;
    if (!state->opened) {
        return H2_DISPLAY_ERR_INVALID_STATE;
    }

    h2_display_rect_t clipped;
    int rc = clip_rect(rect, &clipped);
    if (rc != H2_DISPLAY_OK) {
        return rc;
    }

    size_t src_pixel_size = 0;
    rc = validate_source_bitmap(rect, pixels, stride_bytes, format, &src_pixel_size);
    if (rc != H2_DISPLAY_OK) {
        return rc;
    }
    if (state->dma_buffer == NULL || state->dma_buffer_pixels < (size_t)clipped.width) {
        return H2_DISPLAY_ERR_NO_MEMORY;
    }

    int remaining_rows = clipped.height;
    int y = clipped.y;
    while (remaining_rows > 0) {
        const int chunk_rows = (int)(state->dma_buffer_pixels / (size_t)clipped.width) < remaining_rows
            ? (int)(state->dma_buffer_pixels / (size_t)clipped.width)
            : remaining_rows;
        h2_display_rect_t chunk = clipped;
        chunk.y = y;
        chunk.height = chunk_rows;
        convert_bitmap_to_rgb565(rect, &chunk, pixels, stride_bytes, format, src_pixel_size, state->dma_buffer);
        rc = st7789_set_window(state, chunk.x, chunk.y, chunk.width, chunk.height);
        if (rc != H2_DISPLAY_OK) {
            return rc;
        }
        rc = st7789_color(state, LCD_CMD_RAMWR, state->dma_buffer, (size_t)chunk.width * (size_t)chunk.height * sizeof(uint16_t));
        if (rc != H2_DISPLAY_OK) {
            return rc;
        }
        y += chunk_rows;
        remaining_rows -= chunk_rows;
    }
    return H2_DISPLAY_OK;
}

static int szp_present(void *user) {
    h2_esp_szp_display_state_t *state = (h2_esp_szp_display_state_t *)user;
    return state->opened ? H2_DISPLAY_OK : H2_DISPLAY_ERR_INVALID_STATE;
}

static int szp_set_brightness_percent(void *user, uint32_t percent) {
    h2_esp_szp_display_state_t *state = (h2_esp_szp_display_state_t *)user;
    if (!state->opened) {
        return H2_DISPLAY_ERR_INVALID_STATE;
    }
    return set_backlight_percent(percent);
}

static int szp_open(void *user) {
    h2_esp_szp_display_state_t *state = (h2_esp_szp_display_state_t *)user;
    if (state->opened) {
        return H2_DISPLAY_OK;
    }

    const bool was_initialized = state->initialized;
    int rc = init_display(state);
    if (rc != H2_DISPLAY_OK) {
        return rc;
    }
    if (was_initialized) {
        rc = st7789_cmd(state, LCD_CMD_DISPON, NULL, 0u);
        if (rc == H2_DISPLAY_OK) {
            rc = set_backlight_percent(100u);
        }
        if (rc != H2_DISPLAY_OK) {
            return rc;
        }
    }
    state->opened = true;
    return H2_DISPLAY_OK;
}

static int szp_close(void *user) {
    h2_esp_szp_display_state_t *state = (h2_esp_szp_display_state_t *)user;
    if (!state->opened || state->panel_io == NULL) {
        return H2_DISPLAY_OK;
    }
    int rc = set_backlight_percent(0u);
    int off_rc = st7789_cmd(state, LCD_CMD_DISPOFF, NULL, 0);
    if (off_rc != H2_DISPLAY_OK && rc == H2_DISPLAY_OK) {
        rc = off_rc;
    }
    if (rc == H2_DISPLAY_OK) {
        state->opened = false;
    }
    return rc;
}

int h2_esp_board_display_power_off(void) {
    h2_pal_display_t *display = h2_esp_board_display_if_initialized();
    if (display == NULL) {
        return H2_DISPLAY_OK;
    }
    return h2_pal_display_close(display);
}

h2_pal_display_t *h2_esp_board_display(void) {
    static const h2_pal_display_vtable_t vtable = {
        .open = szp_open,
        .get_info = szp_get_info,
        .draw_bitmap = szp_draw_bitmap,
        .present = szp_present,
        .set_brightness_percent = szp_set_brightness_percent,
        .close = szp_close,
    };
    static h2_pal_display_t display = {
        .user = &s_display_state,
        .vtable = &vtable,
    };
    return &display;
}

h2_pal_display_t *h2_esp_board_display_if_initialized(void) {
    if (!s_display_state.initialized) {
        return NULL;
    }
    return h2_esp_board_display();
}
