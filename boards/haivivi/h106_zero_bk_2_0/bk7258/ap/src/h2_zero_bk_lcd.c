/*
 * H106 ST7789 240x240: SPI0 pins in h2_zero_bk_lcd.h. Init aligned with common
 * ST7789 tables. Full-frame blit follows bk_lcd_spi_frame_display()
 * (lcd_spi_driver.c): stripe by rows so each RAMWR fits one
 * bk_spi_dma_write_bytes (<= SPI_MAX_LENGTH), avoiding CS mid-GRAM.
 */

#include "h2_zero_bk_lcd.h"
#include "h2_zero_bk_spi.h"
#ifdef H2_ZERO_BK_DISPLAY_HOST_TEST
#include "h2_zero_bk_display_test_sdk.h"
#else
#include "gpio_driver.h"
#include <common/bk_include.h>
#include <components/log.h>
#include <driver/gpio.h>
#include <driver/hal/hal_gpio_types.h>
#include <driver/hal/hal_spi_types.h>
#include <driver/spi.h>
#include <os/os.h>
#endif
#include <stddef.h>

#define TAG "h2_zero_bk_lcd"
#define LOGI(...) BK_LOGI(TAG, ##__VA_ARGS__)
#define LOGE(...) BK_LOGE(TAG, ##__VA_ARGS__)

/* Matches GPIO table / SPI0 AF2 */
#define H2_ZERO_BK_LCD_SPI_ID SPI_ID_0
#define H2_ZERO_BK_LCD_PIN_DC GPIO_17
#define H2_ZERO_BK_LCD_PIN_RST GPIO_18

/* Board-selected SPI clock. */
#define H2_ZERO_BK_LCD_SPI_BAUD (40 * 1000000u)

#define ST7789_CMD_RAMWR 0x2C

/* bk_spi_dma_write_bytes chunks at SPI_MAX_LENGTH; stripe so one RAMWR ≤ one
 * DMA transaction */
#define H2_ZERO_BK_LCD_SPI_DMA_PIXEL_MAX 0x10000u
/* GSPI tx_trans_len mask 0xFFF */
#define H2_ZERO_BK_LCD_SPI_CPU_PIXEL_MAX 4095u

static bool s_lcd_inited;
static bool s_spi_owned;
static h2_zero_bk_display_capture_fn s_capture;
static void* s_capture_user;

h2_display_info_t h2_zero_bk_display_set_transfer_capture(
    h2_zero_bk_display_capture_fn capture, void* user) {
  s_capture = capture;
  s_capture_user = user;
  return (h2_display_info_t){H2_ZERO_BK_LCD_WIDTH, H2_ZERO_BK_LCD_HEIGHT,
                             H2_DISPLAY_PIXEL_RGB565};
}
static void lcd_observe_transfer(uint32_t y, uint32_t rows,
                                 const uint8_t* bytes, size_t len) {
  if (s_capture) {
    const h2_display_rect_t rect = {0, (int)y, H2_ZERO_BK_LCD_WIDTH, (int)rows};
    s_capture(s_capture_user, &rect, bytes, len);
  }
}

static int lcd_gpio_high(uint32_t pin) {
  gpio_config_t config = {.io_mode = GPIO_OUTPUT_ENABLE,
                          .pull_mode = GPIO_PULL_DISABLE,
                          .func_mode = GPIO_SECOND_FUNC_DISABLE};
  int rc = gpio_dev_unmap(pin);
  if (rc == BK_OK) rc = bk_gpio_set_config(pin, &config);
  if (rc == BK_OK) rc = bk_gpio_set_output_high(pin);
  return rc;
}
static int lcd_hw_reset(void) {
  int rc = bk_gpio_set_output_high(H2_ZERO_BK_LCD_PIN_DC);
  if (rc == BK_OK) rc = bk_gpio_set_output_high(H2_ZERO_BK_LCD_PIN_RST);
  if (rc != BK_OK) return rc;
  rtos_delay_milliseconds(10);
  rc = bk_gpio_set_output_low(H2_ZERO_BK_LCD_PIN_RST);
  if (rc != BK_OK) return rc;
  rtos_delay_milliseconds(100);
  rc = bk_gpio_set_output_high(H2_ZERO_BK_LCD_PIN_RST);
  if (rc != BK_OK) return rc;
  rtos_delay_milliseconds(120);
  return BK_OK;
}

static int lcd_write_cmd(uint8_t v) {
  int rc = bk_gpio_set_output_low(H2_ZERO_BK_LCD_PIN_DC);
  if (rc == BK_OK) rc = h2_zero_bk_spi_write(H2_ZERO_BK_LCD_SPI_ID, &v, 1);
  BK_LOG_ON_ERR(rc);
  return rc;
}

static int lcd_write_data(const uint8_t* p, uint32_t len) {
  int rc = bk_gpio_set_output_high(H2_ZERO_BK_LCD_PIN_DC);
  if (rc == BK_OK) rc = h2_zero_bk_spi_write(H2_ZERO_BK_LCD_SPI_ID, p, len);
  BK_LOG_ON_ERR(rc);
  return rc;
}

#if CONFIG_SPI_DMA
/* Pixel stream after RAMWR; len must stay within one bk_spi_dma_write_bytes
 * (see stripe helper). */
static int lcd_write_pixels_dma(const uint8_t* p, uint32_t len) {
  int rc = bk_gpio_set_output_high(H2_ZERO_BK_LCD_PIN_DC);
  if (rc == BK_OK) rc = h2_zero_bk_spi_dma_write(H2_ZERO_BK_LCD_SPI_ID, p, len);
  BK_LOG_ON_ERR(rc);
  return rc;
}
#endif

typedef struct {
  uint8_t cmd;
  uint8_t data[16];
  uint8_t len; /* 0xFF = delay ms, data[0]=ms */
} lcd_init_cmd_t;

/* Same command table as lcd_spi_st7789_240x240 / common ST7789 init */
static const lcd_init_cmd_t s_st7789_240_init[] = {
    {0x00, {0x78}, 0xFF},
    {0x11, {0x00}, 0},
    {0x00, {0x78}, 0xFF},
    {0x36, {0x00}, 1},
    {0x3A, {0x55}, 1},
    {0xB0, {0x00, 0xF0}, 2},
    {0xB2, {0x3F, 0x3F, 0x00, 0x33, 0x33}, 5},
    {0xB7, {0x35}, 1},
    {0xBB, {0x2A}, 1},
    {0xC0, {0x2C}, 1},
    {0xC2, {0x01}, 1},
    {0xC3, {0x0B}, 1},
    {0xC4, {0x20}, 1},
    {0xC6, {0x1F}, 1},
    {0xD0, {0xA4, 0xA1}, 2},
    {0xE0,
     {0xD0, 0x01, 0x08, 0x0F, 0x11, 0x2A, 0x36, 0x55, 0x44, 0x3A, 0x0B, 0x06,
      0x11, 0x20},
     14},
    {0xE1,
     {0xD0, 0x02, 0x07, 0x0A, 0x0B, 0x18, 0x34, 0x43, 0x4A, 0x2B, 0x1B, 0x1C,
      0x22, 0x1F},
     14},
    {0x35, {0x00}, 0},
    {0x29, {0x00}, 0},
    {0x21, {0x00}, 0},
};

static int lcd_run_init_table(void) {
  for (unsigned i = 0;
       i < sizeof(s_st7789_240_init) / sizeof(s_st7789_240_init[0]); i++) {
    const lcd_init_cmd_t* c = &s_st7789_240_init[i];

    if (c->len == 0xFF) {
      rtos_delay_milliseconds(c->data[0]);
      continue;
    }
    int rc = lcd_write_cmd(c->cmd);
    if (rc == BK_OK && c->len) rc = lcd_write_data(c->data, c->len);
    if (rc != BK_OK) return rc;
  }
  return BK_OK;
}

static int lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  uint8_t b[4];

  int rc = lcd_write_cmd(0x2A);
  if (rc != BK_OK) return rc;
  b[0] = (uint8_t)(x0 >> 8);
  b[1] = (uint8_t)(x0 & 0xFF);
  b[2] = (uint8_t)(x1 >> 8);
  b[3] = (uint8_t)(x1 & 0xFF);
  rc = lcd_write_data(b, 4);
  if (rc != BK_OK) return rc;

  rc = lcd_write_cmd(0x2B);
  if (rc != BK_OK) return rc;
  b[0] = (uint8_t)(y0 >> 8);
  b[1] = (uint8_t)(y0 & 0xFF);
  b[2] = (uint8_t)(y1 >> 8);
  b[3] = (uint8_t)(y1 & 0xFF);
  rc = lcd_write_data(b, 4);
  if (rc != BK_OK) return rc;
  return BK_OK;
}

/* Align with lcd_spi_driver.c bk_lcd_spi_frame_display (SPI only, no QSPI
 * mapping). */
static int lcd_blit_fullscreen_striped(uint8_t* pix, size_t need) {
  (void)need;
  const uint32_t row_bytes = H2_ZERO_BK_LCD_WIDTH * 2u;
#if CONFIG_SPI_DMA
  const uint32_t max_rows = H2_ZERO_BK_LCD_SPI_DMA_PIXEL_MAX / row_bytes;
#else
  const uint32_t max_rows = H2_ZERO_BK_LCD_SPI_CPU_PIXEL_MAX / row_bytes;
#endif
  for (uint32_t y = 0; y < H2_ZERO_BK_LCD_HEIGHT;) {
    const uint32_t rows = H2_ZERO_BK_LCD_HEIGHT - y < max_rows
                              ? H2_ZERO_BK_LCD_HEIGHT - y
                              : max_rows;
    const uint32_t bytes = rows * row_bytes;
    int rc = lcd_set_window(0, (uint16_t)y, H2_ZERO_BK_LCD_WIDTH - 1,
                            (uint16_t)(y + rows - 1u));
    if (rc == BK_OK) rc = lcd_write_cmd(ST7789_CMD_RAMWR);
#if CONFIG_SPI_DMA
    if (rc == BK_OK)
      rc = lcd_write_pixels_dma(pix + (size_t)y * row_bytes, bytes);
#else
    if (rc == BK_OK) rc = lcd_write_data(pix + (size_t)y * row_bytes, bytes);
#endif
    if (rc != BK_OK) return rc;
    /* The real SDK call has completed DMA and drained the hardware FIFO. */
    lcd_observe_transfer(y, rows, pix + (size_t)y * row_bytes, bytes);
    y += rows;
  }
  return BK_OK;
}

bool h2_zero_bk_lcd_is_inited(void) { return s_lcd_inited; }

int h2_zero_bk_lcd_init(void) {
  if (s_lcd_inited) return BK_OK;
  if (s_spi_owned && h2_zero_bk_lcd_deinit() != BK_OK) return -1;
  int rc = bk_gpio_driver_init();
  if (rc == BK_OK) rc = gpio_spi_sel(GPIO_SPI_MAP_MODE0);
  /* SPI0 maps its fourth pin too. Restore DC after native SPI init. */
  if (rc == BK_OK) rc = lcd_gpio_high(H2_ZERO_BK_LCD_PIN_RST);
  if (rc == BK_OK) rc = bk_spi_driver_init();
  if (rc != BK_OK) return rc;
  spi_config_t config = {.role = SPI_ROLE_MASTER,
                         .bit_width = SPI_BIT_WIDTH_8BITS,
                         .polarity = SPI_POLARITY_LOW,
                         .phase = SPI_PHASE_1ST_EDGE,
                         .wire_mode = SPI_4WIRE_MODE,
                         .baud_rate = H2_ZERO_BK_LCD_SPI_BAUD,
                         .bit_order = SPI_MSB_FIRST};
#if CONFIG_SPI_DMA
  config.dma_mode = SPI_DMA_MODE_ENABLE;
  config.spi_tx_dma_width = DMA_DATA_WIDTH_8BITS;
  config.spi_rx_dma_width = DMA_DATA_WIDTH_8BITS;
#endif
  s_spi_owned = true;
  rc = bk_spi_init(H2_ZERO_BK_LCD_SPI_ID, &config);
  if (rc == BK_OK) rc = lcd_gpio_high(H2_ZERO_BK_LCD_PIN_DC);
  if (rc == BK_OK) rc = lcd_gpio_high(H2_ZERO_BK_LCD_PIN_RST);
  if (rc == BK_OK) rc = lcd_hw_reset();
  if (rc == BK_OK) rc = lcd_run_init_table();
  if (rc == BK_OK)
    rc = lcd_set_window(0, 0, H2_ZERO_BK_LCD_WIDTH - 1,
                        H2_ZERO_BK_LCD_HEIGHT - 1);
  if (rc != BK_OK) {
    (void)h2_zero_bk_lcd_deinit();
    return rc;
  }
  rtos_delay_milliseconds(20);
  s_lcd_inited = true;
  LOGI("ST7789 ready, SPI %u Hz\r\n", (unsigned)H2_ZERO_BK_LCD_SPI_BAUD);
  return BK_OK;
}
int h2_zero_bk_lcd_deinit(void) {
  if (!s_spi_owned) return BK_OK;
  int rc = bk_spi_deinit(H2_ZERO_BK_LCD_SPI_ID);
  if (rc != BK_OK) return rc;
  s_spi_owned = false;
  s_lcd_inited = false;
  return BK_OK;
}

void h2_zero_bk_lcd_fill_rgb565(uint16_t color) {
  uint8_t row[H2_ZERO_BK_LCD_WIDTH * 2];
  uint8_t hi = (uint8_t)(color >> 8);
  uint8_t lo = (uint8_t)(color & 0xFF);

  if (!s_lcd_inited) return;

  for (int i = 0; i < H2_ZERO_BK_LCD_WIDTH * 2; i += 2) {
    row[i] = hi;
    row[i + 1] = lo;
  }

#if CONFIG_SPI_DMA
  /* One RAMWR must match one CS transaction; do not split one full-window RAMWR
   * across many DMA bursts. */
  for (uint16_t y = 0; y < H2_ZERO_BK_LCD_HEIGHT; y++) {
    lcd_set_window(0, y, H2_ZERO_BK_LCD_WIDTH - 1, y);
    lcd_write_cmd(ST7789_CMD_RAMWR);
    lcd_write_pixels_dma(row, sizeof(row));
  }
#else
  for (uint16_t y = 0; y < H2_ZERO_BK_LCD_HEIGHT; y++) {
    lcd_set_window(0, y, H2_ZERO_BK_LCD_WIDTH - 1, y);
    lcd_write_cmd(ST7789_CMD_RAMWR);
    lcd_write_data(row, sizeof(row));
  }
#endif
}

int h2_zero_bk_lcd_blit_rgb565_fullscreen(uint8_t* pix, size_t len,
                                          int swap_pairs) {
  const size_t need =
      (size_t)H2_ZERO_BK_LCD_WIDTH * (size_t)H2_ZERO_BK_LCD_HEIGHT * 2u;

  if (!s_lcd_inited || !pix || len < need) return -1;

  if (swap_pairs) {
    for (size_t i = 0; i < need; i += 2) {
      uint8_t t = pix[i];

      pix[i] = pix[i + 1];
      pix[i + 1] = t;
    }
  }

  return lcd_blit_fullscreen_striped(pix, need);
}
