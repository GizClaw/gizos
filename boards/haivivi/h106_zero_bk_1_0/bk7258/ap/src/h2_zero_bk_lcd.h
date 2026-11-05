#pragma once

#include "h2_zero_bk_display_capture.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Zero BK 1.0 ST7789 240x240, SPI0 (AF2) per schematic:
 *   GPIO14 SCK, GPIO15 CSN, GPIO16 MOSI, GPIO17 DCX (GPIO, no MISO read),
 *   GPIO18 RST, GPIO19 backlight owned by the board Display provider.
 *
 * SPI blit follows sdk lcd_spi_driver.c bk_lcd_spi_frame_display (pure SPI, no
 * QSPI): stripe by full rows so each RAMWR fits one bk_spi_dma_write_bytes
 * (≤64KiB). Without that, a full-frame RAMWR is split inside the driver and CS
 * can toggle mid-GRAM. Non-DMA path stripes to ≤4095 B per SPI xfer. swap_pairs
 * may reorder pix once per blit.
 */

#define H2_ZERO_BK_LCD_WIDTH 240
#define H2_ZERO_BK_LCD_HEIGHT 240

int h2_zero_bk_lcd_init(void);
/** Release this LCD's SPI channel. Returns the native error on failure and
 * retains cleanup ownership for retry; repeated successful deinit is a no-op.
 */
int h2_zero_bk_lcd_deinit(void);
bool h2_zero_bk_lcd_is_inited(void);

/** Full-screen RGB565 fill (big-endian matches ST7789 COLMOD 0x55) */
void h2_zero_bk_lcd_fill_rgb565(uint16_t color);

/**
 * Full-screen RGB565 from linear buffer (row-major).
 * @param swap_pairs if non-zero, swap each pixel's two bytes in
 * pix[0..width*height*2) (LE file → ST7789 wire); buffer must be writable.
 * @return 0 on success, -1 if LCD not ready or buffer too small
 */
int h2_zero_bk_lcd_blit_rgb565_fullscreen(uint8_t* pix, size_t len,
                                          int swap_pairs);
