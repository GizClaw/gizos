#include "h2_esp_board_private.h"
#include "h2_esp_board_internal.h"
#include "h2_esp_board.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_sh8601.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define LCD_HOST SPI2_HOST
#define LCD_WIDTH 368
#define LCD_HEIGHT 448
#define LCD_CS_GPIO GPIO_NUM_12
#define LCD_PCLK_GPIO GPIO_NUM_11
#define LCD_DATA0_GPIO GPIO_NUM_4
#define LCD_DATA1_GPIO GPIO_NUM_5
#define LCD_DATA2_GPIO GPIO_NUM_6
#define LCD_DATA3_GPIO GPIO_NUM_7
#define LCD_RST_GPIO GPIO_NUM_NC
#define LCD_CONTROL_RESET_MASK (1u << 0)
#define LCD_CONTROL_POWER_MASK (1u << 1)
#define LCD_CONTROL_PANEL_MASK (LCD_CONTROL_RESET_MASK | LCD_CONTROL_POWER_MASK)
#define LCD_BITS_PER_PIXEL 16
#define LCD_DRAW_ROWS 64
#define LCD_DMA_BUFFER_PIXELS (LCD_WIDTH * LCD_DRAW_ROWS)
#define LCD_DMA_BUFFER_BYTES (LCD_DMA_BUFFER_PIXELS * sizeof(uint16_t))
#define LCD_OPCODE_WRITE_CMD 0x02

#define SH8601_CMD_SLEEP_IN 0x10
#define SH8601_CMD_SLEEP_OUT 0x11
#define SH8601_CMD_DISPLAY_OFF 0x28
#define SH8601_CMD_DISPLAY_BRIGHTNESS 0x51

typedef struct h2_esp_amoled_display_state {
    esp_lcd_panel_io_handle_t panel_io;
    esp_lcd_panel_handle_t panel;
    uint16_t *dma_buffer;
    size_t dma_buffer_pixels;
    bool initialized;
    bool opened;
} h2_esp_amoled_display_state_t;

static const char *TAG = "h2_esp_amoled";
static h2_esp_amoled_display_state_t s_display_state;
static h2_esp_board_display_config_t s_display_config;

h2_pal_result_t h2_esp_board_display_configure(
    const h2_esp_board_display_config_t *config) {
    if (!h2_esp_board_display_config_is_valid(config)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (!h2_esp_board_display_config_may_apply(
            s_display_state.panel_io != NULL || s_display_state.initialized ||
            s_display_state.opened)) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    s_display_config = *config;
    return H2_PAL_OK;
}

static const sh8601_lcd_init_cmd_t s_amoled_init_cmds[] = {
    { SH8601_CMD_SLEEP_OUT, NULL, 0, 120 },
    { 0x44, (uint8_t[]){ 0x01, 0xd1 }, 2, 0 },
    { 0x35, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x53, (uint8_t[]){ 0x20 }, 1, 10 },
    { 0x2a, (uint8_t[]){ 0x00, 0x00, 0x01, 0x6f }, 4, 0 },
    { 0x2b, (uint8_t[]){ 0x00, 0x00, 0x01, 0xbf }, 4, 0 },
    { SH8601_CMD_DISPLAY_BRIGHTNESS, (uint8_t[]){ 0x00 }, 1, 10 },
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

static int drain_panel_io(h2_esp_amoled_display_state_t *state) {
    esp_err_t err = esp_lcd_panel_io_tx_param(state->panel_io, -1, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel io drain failed: %s", esp_err_to_name(err));
    }
    return esp_result(err);
}

static esp_err_t tx_qspi_param(h2_esp_amoled_display_state_t *state, uint8_t command, const void *data, size_t data_len) {
    int encoded_command = ((int)LCD_OPCODE_WRITE_CMD << 24) | ((int)command << 8);
    return esp_lcd_panel_io_tx_param(state->panel_io, encoded_command, data, data_len);
}

static int set_brightness(h2_esp_amoled_display_state_t *state, uint8_t brightness) {
    esp_err_t err = tx_qspi_param(state, SH8601_CMD_DISPLAY_BRIGHTNESS, &brightness, sizeof(brightness));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set display brightness failed: %s", esp_err_to_name(err));
    }
    return esp_result(err);
}

static int init_panel_power_control(h2_esp_amoled_display_state_t *state) {
    (void)state;
    esp_err_t err = h2_esp_amoled_board_io_update_outputs(
        LCD_CONTROL_PANEL_MASK, 0u);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display control io expander configure failed: %s", esp_err_to_name(err));
    }
    return esp_result(err);
}

static int set_panel_power(h2_esp_amoled_display_state_t *state, bool enabled) {
    int rc = init_panel_power_control(state);
    if (rc != H2_DISPLAY_OK) {
        return rc;
    }

    esp_err_t err = h2_esp_amoled_board_io_update_outputs(
        LCD_CONTROL_PANEL_MASK,
        enabled ? LCD_CONTROL_PANEL_MASK : 0u);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set display panel power failed: %s", esp_err_to_name(err));
    }
    return esp_result(err);
}

static int reset_panel_power(h2_esp_amoled_display_state_t *state) {
    int rc = set_panel_power(state, false);
    if (rc != H2_DISPLAY_OK) {
        return rc;
    }
    vTaskDelay(pdMS_TO_TICKS(300));
    rc = set_panel_power(state, true);
    if (rc != H2_DISPLAY_OK) {
        return rc;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    return H2_DISPLAY_OK;
}

static int init_panel_io(h2_esp_amoled_display_state_t *state) {
    const spi_bus_config_t buscfg = SH8601_PANEL_BUS_QSPI_CONFIG(
        LCD_PCLK_GPIO,
        LCD_DATA0_GPIO,
        LCD_DATA1_GPIO,
        LCD_DATA2_GPIO,
        LCD_DATA3_GPIO,
        LCD_DMA_BUFFER_BYTES);
    esp_err_t err = spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return esp_result(err);
    }
    if (state->panel_io != NULL) {
        return H2_DISPLAY_OK;
    }
    esp_lcd_panel_io_spi_config_t io_config = SH8601_PANEL_IO_QSPI_CONFIG(
        LCD_CS_GPIO,
        NULL,
        NULL);
    if (s_display_config.pclk_hz != 0u) {
        io_config.pclk_hz = (int)s_display_config.pclk_hz;
    }
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &state->panel_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi failed: %s", esp_err_to_name(err));
        return esp_result(err);
    }
    return H2_DISPLAY_OK;
}

static int init_display(h2_esp_amoled_display_state_t *state) {
    if (state->initialized) {
        return H2_DISPLAY_OK;
    }

    ESP_LOGI(TAG, "init SH8601 QSPI panel");
    int rc = reset_panel_power(state);
    if (rc != H2_DISPLAY_OK) {
        return rc;
    }
    rc = init_panel_io(state);
    if (rc != H2_DISPLAY_OK) {
        return rc;
    }
    if (state->dma_buffer == NULL) {
        state->dma_buffer = (uint16_t *)heap_caps_malloc(LCD_DMA_BUFFER_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (state->dma_buffer == NULL) {
            ESP_LOGE(TAG, "display dma buffer alloc failed bytes=%u", (unsigned)LCD_DMA_BUFFER_BYTES);
            return H2_DISPLAY_ERR_NO_MEMORY;
        }
        state->dma_buffer_pixels = LCD_DMA_BUFFER_PIXELS;
    }

    sh8601_vendor_config_t vendor_config = {
        .init_cmds = s_amoled_init_cmds,
        .init_cmds_size = sizeof(s_amoled_init_cmds) / sizeof(s_amoled_init_cmds[0]),
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = LCD_BITS_PER_PIXEL,
        .reset_gpio_num = LCD_RST_GPIO,
        .vendor_config = &vendor_config,
    };
    esp_err_t err = esp_lcd_new_panel_sh8601(state->panel_io, &panel_config, &state->panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_sh8601 failed: %s", esp_err_to_name(err));
        return esp_result(err);
    }

    err = esp_lcd_panel_reset(state->panel);
    if (err == ESP_OK) {
        err = esp_lcd_panel_init(state->panel);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_disp_on_off(state->panel, true);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (err == ESP_OK) {
        int rc = set_brightness(state, 0xff);
        err = rc == H2_DISPLAY_OK ? ESP_OK : ESP_FAIL;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel init failed: %s", esp_err_to_name(err));
        return esp_result(err);
    }

    state->initialized = true;
    return H2_DISPLAY_OK;
}

static int clip_rect(const h2_display_rect_t *rect, h2_display_rect_t *clipped) {
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
    if (src_rect == NULL || pixels == NULL || src_pixel_size == NULL || src_rect->width <= 0 || src_rect->height <= 0) {
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

static void convert_chunk_to_rgb565(
    const h2_display_rect_t *src_rect,
    const h2_display_rect_t *chunk,
    const void *pixels,
    size_t stride_bytes,
    h2_display_pixel_format_t format,
    size_t src_pixel_size,
    uint16_t *out) {
    const uint8_t *src = (const uint8_t *)pixels;
    src += (size_t)(chunk->y - src_rect->y) * stride_bytes;
    src += (size_t)(chunk->x - src_rect->x) * src_pixel_size;

    for (int row = 0; row < chunk->height; ++row) {
        uint16_t *dst = out + (size_t)row * (size_t)chunk->width;
        const uint8_t *src_row = src + (size_t)row * stride_bytes;
        if (format == H2_DISPLAY_PIXEL_RGB565) {
            const uint16_t *src16 = (const uint16_t *)src_row;
            for (int col = 0; col < chunk->width; ++col) {
                dst[col] = rgb565_to_panel(src16[col]);
            }
        } else if (format == H2_DISPLAY_PIXEL_RGB888) {
            for (int col = 0; col < chunk->width; ++col) {
                dst[col] = rgb565_to_panel(rgb888_to_rgb565(src_row + (size_t)col * 3u));
            }
        } else {
            const uint16_t *src16 = (const uint16_t *)src_row;
            for (int col = 0; col < chunk->width; ++col) {
                dst[col] = rgb565_to_panel(rgb444_to_rgb565(src16[col]));
            }
        }
    }
}

static int amoled_get_info(void *user, h2_display_info_t *info) {
    h2_esp_amoled_display_state_t *state = (h2_esp_amoled_display_state_t *)user;
    if (!state->opened) {
        return H2_DISPLAY_ERR_INVALID_STATE;
    }

    info->width = LCD_WIDTH;
    info->height = LCD_HEIGHT;
    info->native_format = H2_DISPLAY_PIXEL_RGB565;
    return H2_DISPLAY_OK;
}

static int amoled_draw_bitmap(
    void *user,
    const h2_display_rect_t *rect,
    const void *pixels,
    size_t stride_bytes,
    h2_display_pixel_format_t format) {
    h2_esp_amoled_display_state_t *state = (h2_esp_amoled_display_state_t *)user;
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

    const size_t max_chunk_pixels = state->dma_buffer_pixels;
    if (state->dma_buffer == NULL || max_chunk_pixels == 0u || (size_t)clipped.width > max_chunk_pixels) {
        return H2_DISPLAY_ERR_NO_MEMORY;
    }
    int max_chunk_rows = (int)(max_chunk_pixels / (size_t)clipped.width);
    if (max_chunk_rows <= 0 || max_chunk_rows > LCD_DRAW_ROWS) {
        max_chunk_rows = LCD_DRAW_ROWS;
    }

    int y = clipped.y;
    const int y_end = clipped.y + clipped.height;
    while (y < y_end) {
        h2_display_rect_t chunk = clipped;
        chunk.y = y;
        chunk.height = y_end - y;
        if (chunk.height > max_chunk_rows) {
            chunk.height = max_chunk_rows;
        }

        convert_chunk_to_rgb565(rect, &chunk, pixels, stride_bytes, format, src_pixel_size, state->dma_buffer);
        esp_err_t err = esp_lcd_panel_draw_bitmap(
            state->panel,
            chunk.x,
            chunk.y,
            chunk.x + chunk.width,
            chunk.y + chunk.height,
            state->dma_buffer);
        if (err != ESP_OK) {
            return esp_result(err);
        }
        rc = drain_panel_io(state);
        if (rc != H2_DISPLAY_OK) {
            return rc;
        }
        y += chunk.height;
    }
    return H2_DISPLAY_OK;
}

static int amoled_present(void *user) {
    h2_esp_amoled_display_state_t *state = (h2_esp_amoled_display_state_t *)user;
    return state->opened ? H2_DISPLAY_OK : H2_DISPLAY_ERR_INVALID_STATE;
}

static int amoled_set_brightness_percent(void *user, uint32_t percent) {
    h2_esp_amoled_display_state_t *state = (h2_esp_amoled_display_state_t *)user;
    if (!state->opened) {
        return H2_DISPLAY_ERR_INVALID_STATE;
    }
    if (percent > 100u) {
        percent = 100u;
    }
    return set_brightness(state, (uint8_t)((percent * 255u) / 100u));
}

static int amoled_open(void *user) {
    h2_esp_amoled_display_state_t *state = (h2_esp_amoled_display_state_t *)user;
    if (state->opened) {
        return H2_DISPLAY_OK;
    }

    const bool was_initialized = state->initialized;
    int rc = init_display(state);
    if (rc != H2_DISPLAY_OK) {
        return rc;
    }
    if (was_initialized) {
        esp_err_t err = esp_lcd_panel_disp_on_off(state->panel, true);
        if (err != ESP_OK) {
            return esp_result(err);
        }
        rc = set_brightness(state, 0xffu);
        if (rc != H2_DISPLAY_OK) {
            return rc;
        }
    }
    state->opened = true;
    return H2_DISPLAY_OK;
}

static int amoled_close(void *user) {
    h2_esp_amoled_display_state_t *state = (h2_esp_amoled_display_state_t *)user;
    if (!state->opened || state->panel == NULL) {
        return H2_DISPLAY_OK;
    }
    int rc = set_brightness(state, 0u);
    esp_err_t err = esp_lcd_panel_disp_on_off(state->panel, false);
    if (err != ESP_OK && rc == H2_DISPLAY_OK) {
        rc = esp_result(err);
    }
    if (rc == H2_DISPLAY_OK) {
        state->opened = false;
    }
    return rc;
}

int h2_esp_board_display_power_off(void) {
    h2_esp_amoled_display_state_t *state = &s_display_state;
    int rc = init_panel_io(state);
    if (rc != H2_DISPLAY_OK) {
        return rc;
    }

    uint8_t brightness = 0u;
    esp_err_t err = tx_qspi_param(state, SH8601_CMD_DISPLAY_BRIGHTNESS, &brightness, sizeof(brightness));
    if (err == ESP_OK) {
        err = tx_qspi_param(state, SH8601_CMD_DISPLAY_OFF, NULL, 0);
    }
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(10));
        err = tx_qspi_param(state, SH8601_CMD_SLEEP_IN, NULL, 0);
    }
    if (err == ESP_OK) {
        rc = drain_panel_io(state);
    } else {
        rc = esp_result(err);
    }
    int power_rc = set_panel_power(state, false);
    if (rc == H2_DISPLAY_OK) {
        rc = power_rc;
    }
    return rc;
}

h2_pal_display_t *h2_esp_board_display(void) {
    static const h2_pal_display_vtable_t vtable = {
        .open = amoled_open,
        .get_info = amoled_get_info,
        .draw_bitmap = amoled_draw_bitmap,
        .present = amoled_present,
        .set_brightness_percent = amoled_set_brightness_percent,
        .close = amoled_close,
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
