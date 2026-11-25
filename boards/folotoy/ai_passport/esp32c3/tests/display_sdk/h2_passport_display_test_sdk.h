#ifndef H2_PASSPORT_DISPLAY_TEST_SDK_H
#define H2_PASSPORT_DISPLAY_TEST_SDK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
typedef void* esp_lcd_panel_handle_t;
typedef void* esp_lcd_panel_io_handle_t;
typedef int esp_lcd_spi_bus_handle_t;
enum {
  ESP_OK = 0,
  ESP_FAIL = -1,
  ESP_ERR_NO_MEM = 0x101,
  SPI2_HOST = 2,
  SPI_DMA_CH_AUTO = 3,
  LEDC_LOW_SPEED_MODE = 0,
  LEDC_CHANNEL_0 = 0,
  LEDC_TIMER_0 = 0,
  LEDC_TIMER_10_BIT = 10,
  LEDC_AUTO_CLK = 0,
  LCD_RGB_ELEMENT_ORDER_RGB = 0,
  LCD_RGB_DATA_ENDIAN_LITTLE = 0,
  MALLOC_CAP_DMA = 1,
  MALLOC_CAP_INTERNAL = 2,
};

typedef struct {
  int mosi_io_num, miso_io_num, sclk_io_num;
  int quadwp_io_num, quadhd_io_num, max_transfer_sz;
} spi_bus_config_t;
typedef struct {
  int dc_gpio_num, cs_gpio_num, pclk_hz, spi_mode;
  int trans_queue_depth, lcd_cmd_bits, lcd_param_bits;
} esp_lcd_panel_io_spi_config_t;
typedef struct {
  int reset_gpio_num, rgb_ele_order, bits_per_pixel, data_endian;
} esp_lcd_panel_dev_config_t;
typedef struct {
  int speed_mode, duty_resolution, timer_num, freq_hz, clk_cfg;
} ledc_timer_config_t;
typedef struct {
  int gpio_num, speed_mode, channel, timer_sel;
  unsigned duty;
} ledc_channel_config_t;

void* heap_caps_malloc(size_t size, unsigned caps);
void heap_caps_free(void* pointer);
esp_err_t spi_bus_initialize(int host, const spi_bus_config_t* config, int dma);
esp_err_t spi_bus_free(int host);
esp_err_t esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t bus,
                                   const esp_lcd_panel_io_spi_config_t* config,
                                   esp_lcd_panel_io_handle_t* out);
esp_err_t esp_lcd_new_panel_st7789(esp_lcd_panel_io_handle_t io,
                                   const esp_lcd_panel_dev_config_t* config,
                                   esp_lcd_panel_handle_t* out);
esp_err_t esp_lcd_panel_del(esp_lcd_panel_handle_t handle);
esp_err_t esp_lcd_panel_io_del(esp_lcd_panel_io_handle_t handle);
esp_err_t esp_lcd_panel_reset(esp_lcd_panel_handle_t handle);
esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t handle);
esp_err_t esp_lcd_panel_invert_color(esp_lcd_panel_handle_t handle,
                                     bool enabled);
esp_err_t esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t handle,
                                    bool enabled);
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t handle, int x1,
                                    int y1, int x2, int y2, const void* pixels);
esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t handle,
                                    int command, const void* data, size_t size);
esp_err_t ledc_timer_config(const ledc_timer_config_t* config);
esp_err_t ledc_channel_config(const ledc_channel_config_t* config);
esp_err_t ledc_timer_pause(int mode, int timer);
esp_err_t ledc_stop(int mode, int channel, unsigned idle);
esp_err_t ledc_set_duty(int mode, int channel, uint32_t duty);
esp_err_t ledc_update_duty(int mode, int channel);

#endif
