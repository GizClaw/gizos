#include "h2_pal_display_e2e.h"
#include "h2_zero_bk_display_capture.h"
#include "h2_zero_bk_display_test_sdk.h"
#include "h2_zero_bk_lcd.h"
#include "h2_zero_bk_spi.h"
#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

h2_pal_display_t* h2_bk7258_board_display(void);
enum fault {
  SPI_INIT,
  SPI_COMMAND,
  SPI_DMA,
  SPI_CLOSE,
  GPIO,
  PWM_INIT,
  PWM_START,
  PWM_UPDATE,
  PWM_CLOSE,
  FAULT_COUNT
};
static int faults[FAULT_COUNT], spi_owned, pwm_owned, pwm_running, dc;
static unsigned gpio19, duty, period, allocations, captures, dma_calls;
static uint8_t command;
static unsigned window[4];
static const void* last_dma;
static size_t last_dma_len;
static uint16_t actual[H2_ZERO_BK_LCD_WIDTH * H2_ZERO_BK_LCD_HEIGHT];
static uint16_t captured[H2_ZERO_BK_LCD_WIDTH * H2_ZERO_BK_LCD_HEIGHT];
static int fault(enum fault f) {
  if (!faults[f]) return 0;
  --faults[f];
  return -42;
}
int rtos_init_mutex(beken_mutex_t* m) {
  *m = (void*)1;
  return 0;
}
int rtos_lock_mutex(beken_mutex_t* m) {
  assert(*m);
  return 0;
}
int rtos_unlock_mutex(beken_mutex_t* m) {
  assert(*m);
  return 0;
}
void rtos_delay_milliseconds(uint32_t ms) { (void)ms; }
int bk_gpio_driver_init(void) { return 0; }
int gpio_spi_sel(uint32_t mode) {
  assert(mode == GPIO_SPI_MAP_MODE0);
  return 0;
}
int gpio_dev_unmap(uint32_t pin) {
  (void)pin;
  return 0;
}
int bk_gpio_set_config(uint32_t pin, const gpio_config_t* c) {
  assert(c->io_mode == GPIO_OUTPUT_ENABLE);
  return pin == 19 ? fault(GPIO) : 0;
}
int bk_gpio_set_output_high(uint32_t pin) {
  if (pin == 17) dc = 1;
  if (pin == 19) gpio19 = 1;
  return 0;
}
int bk_gpio_set_output_low(uint32_t pin) {
  if (pin == 17) dc = 0;
  if (pin == 19) gpio19 = 0;
  return 0;
}
int bk_spi_driver_init(void) { return 0; }
int bk_spi_init(uint32_t id, const spi_config_t* c) {
  assert(id == 0 && c->dma_mode == SPI_DMA_MODE_ENABLE && !spi_owned);
  spi_owned = 1;
  return fault(SPI_INIT);
}
int bk_spi_deinit(uint32_t id) {
  assert(id == 0);
  int rc = fault(SPI_CLOSE);
  if (!rc) spi_owned = 0;
  return rc;
}
int bk_spi_write_bytes(uint32_t id, const void* data, uint32_t len) {
  assert(id == 0 && spi_owned);
  int rc = fault(SPI_COMMAND);
  if (rc) return rc;
  const uint8_t* p = data;
  if (!dc) {
    assert(len == 1);
    command = *p;
  } else if (command == 0x2a || command == 0x2b) {
    assert(len == 4);
    unsigned n = command == 0x2a ? 0 : 2;
    window[n] = ((unsigned)p[0] << 8) | p[1];
    window[n + 1] = ((unsigned)p[2] << 8) | p[3];
  }
  return 0;
}
int bk_spi_dma_write_bytes(spi_id_t id, const void* data, uint32_t len) {
  assert(id == 0 && spi_owned && dc && command == 0x2c);
  int rc = fault(SPI_DMA);
  if (rc) return rc;
  unsigned width = window[1] - window[0] + 1,
           height = window[3] - window[2] + 1;
  assert(len == width * height * 2 && len <= 65536 &&
         window[1] < H2_ZERO_BK_LCD_WIDTH && window[3] < H2_ZERO_BK_LCD_HEIGHT);
  const uint8_t* p = data;
  for (unsigned y = 0; y < height; ++y)
    for (unsigned x = 0; x < width; ++x) {
      size_t i = ((size_t)y * width + x) * 2;
      actual[(window[2] + y) * H2_ZERO_BK_LCD_WIDTH + window[0] + x] =
          (uint16_t)((p[i] << 8) | p[i + 1]);
    }
  last_dma = data;
  last_dma_len = len;
  ++dma_calls;
  return 0;
}
int bk_pwm_driver_init(void) { return 0; }
int bk_pwm_init(uint32_t ch, const pwm_init_config_t* c) {
  assert(ch == PWM_CH_1 && !pwm_owned);
  pwm_owned = 1;
  period = c->period_cycle;
  duty = c->duty_cycle;
  return fault(PWM_INIT);
}
int bk_pwm_start(uint32_t ch) {
  assert(ch == PWM_CH_1 && pwm_owned);
  int rc = fault(PWM_START);
  if (!rc) pwm_running = 1;
  return rc;
}
int bk_pwm_deinit(uint32_t ch) {
  assert(ch == PWM_CH_1);
  int rc = fault(PWM_CLOSE);
  if (!rc) {
    pwm_owned = 0;
    pwm_running = 0;
  }
  return rc;
}
int bk_pwm_set_period_duty(uint32_t ch, pwm_period_duty_config_t* c) {
  assert(ch == PWM_CH_1 && pwm_owned && pwm_running);
  int rc = fault(PWM_UPDATE);
  if (!rc) {
    period = c->period_cycle;
    duty = c->duty_cycle;
  }
  return rc;
}
static void capture(void* user, const h2_display_rect_t* rect, const uint8_t* p,
                    size_t len) {
  (void)user;
  assert(dma_calls > captures && p == last_dma && len == last_dma_len);
  assert(rect->x == 0 && rect->y == (int)window[2] &&
         rect->width == H2_ZERO_BK_LCD_WIDTH);
  for (int y = 0; y < rect->height; ++y)
    for (int x = 0; x < rect->width; ++x) {
      size_t i = ((size_t)y * rect->width + x) * 2;
      captured[(size_t)(rect->y + y) * H2_ZERO_BK_LCD_WIDTH + x] =
          (uint16_t)((p[i] << 8) | p[i + 1]);
    }
  ++captures;
}
static int observe(void* u, const uint16_t* expected, int w, int h,
                   uint32_t brightness, const char* id) {
  (void)u;
  (void)id;
  assert(w == H2_ZERO_BK_LCD_WIDTH && h == H2_ZERO_BK_LCD_HEIGHT);
  assert(captures && memcmp(expected, actual, sizeof(actual)) == 0 &&
         memcmp(captured, actual, sizeof(actual)) == 0);
  assert(h2_zero_bk_display_backlight_percent() == brightness);
  if (brightness == 0 || brightness == 100)
    assert(!pwm_owned && gpio19 == (brightness != 0));
  else
    assert(pwm_owned && pwm_running && duty * 100u == period * brightness);
  return 0;
}
static void* allocate(void* u, size_t len) {
  (void)u;
  void* p = malloc(len);
  if (p) ++allocations;
  return p;
}
static void release(void* u, void* p) {
  (void)u;
  if (p) {
    assert(allocations);
    --allocations;
    free(p);
  }
}
static void* resize(void* u, void* p, size_t len) {
  (void)u;
  if (!p) return allocate(u, len);
  return realloc(p, len);
}
int main(void) {
  assert(h2_zero_bk_spi_init() == 0);
  const h2_pal_mem_vtable_t vt = {allocate, resize, release};
  const h2_pal_mem_api_t mem = {NULL, &vt};
  h2_runtime_t runtime = {0};
  runtime.mem = &mem;
  runtime.display = h2_bk7258_board_display();
  h2_display_info_t info =
      h2_zero_bk_display_set_transfer_capture(capture, NULL);
  assert(info.native_format == H2_DISPLAY_PIXEL_RGB565);
  const h2_pal_display_e2e_config_t cfg = {
      .supported_formats = 7, .clips_rectangles = 1, .observe = observe};
  h2_pal_display_e2e_result_t result;
  assert(h2_pal_display_e2e_run(&runtime, &cfg, &result) == 0);
  assert(result.qualified && result.complete &&
         result.passed == H2_PAL_DISPLAY_E2E_CASE_COUNT && !allocations &&
         !spi_owned && !pwm_owned);
  for (enum fault f = SPI_INIT; f <= SPI_COMMAND; ++f) {
    faults[f] = 1;
    assert(h2_pal_display_open(runtime.display) == H2_DISPLAY_ERR_IO);
    assert(!spi_owned && h2_pal_display_get_info(runtime.display, &info) ==
                             H2_DISPLAY_ERR_INVALID_STATE);
  }
  faults[GPIO] = 1;
  assert(h2_pal_display_open(runtime.display) == H2_DISPLAY_ERR_IO);
  assert(!spi_owned);
  assert(h2_pal_display_open(runtime.display) == 0);
  unsigned before = captures;
  faults[SPI_DMA] = 1;
  assert(h2_pal_display_present(runtime.display) == H2_DISPLAY_ERR_IO &&
         captures == before);
  for (enum fault f = PWM_INIT; f <= PWM_START; ++f) {
    faults[f] = 1;
    assert(h2_pal_display_set_brightness_percent(runtime.display, 50) ==
           H2_DISPLAY_ERR_IO);
    assert(!pwm_owned && h2_zero_bk_display_backlight_percent() == 100);
  }
  faults[PWM_START] = 1;
  faults[PWM_CLOSE] = 1;
  assert(h2_pal_display_set_brightness_percent(runtime.display, 50) ==
             H2_DISPLAY_ERR_IO &&
         pwm_owned);
  assert(h2_pal_display_set_brightness_percent(runtime.display, 50) == 0 &&
         pwm_running);
  faults[PWM_UPDATE] = 1;
  assert(h2_pal_display_set_brightness_percent(runtime.display, 25) ==
         H2_DISPLAY_ERR_IO);
  assert(h2_zero_bk_display_backlight_percent() == 50 && duty * 2 == period);
  faults[PWM_CLOSE] = 1;
  assert(h2_pal_display_close(runtime.display) == H2_DISPLAY_ERR_IO);
  assert(h2_pal_display_close(runtime.display) == 0 && !pwm_owned &&
         !spi_owned);
  assert(h2_pal_display_open(runtime.display) == 0);
  faults[SPI_CLOSE] = 1;
  assert(h2_pal_display_close(runtime.display) == H2_DISPLAY_ERR_IO &&
         spi_owned);
  assert(h2_pal_display_close(runtime.display) == 0 && !spi_owned);
  (void)h2_zero_bk_display_set_transfer_capture(NULL, NULL);
  return 0;
}
