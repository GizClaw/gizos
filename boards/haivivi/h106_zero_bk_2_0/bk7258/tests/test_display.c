#include "h2_pal_display_e2e.h"
#include "h2_zero_bk_display_capture.h"
#include "h2_zero_bk_display_test_sdk.h"
#include "h2_zero_bk_board_config.h"
#include "h2_zero_bk_led_test_sdk.h"
#include "h2_zero_bk_lcd.h"
#include "h2_zero_bk_spi.h"
#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

h2_pal_display_t* h2_bk7258_board_display(void);
const h2_pal_led_api_t* h2_bk7258_board_led_api(void);
enum fault {
  SPI_INIT,
  SPI_COMMAND,
  SPI_PIXELS,
  SPI_CLOSE,
  GPIO,
  PWM_INIT,
  PWM_START,
  PWM_UPDATE,
  PWM_CLOSE,
  FAULT_COUNT
};
static int faults[FAULT_COUNT], spi_owned, pwm_owned, pwm_running, dc;
static unsigned gpio19, duty, period, allocations, captures, pixel_calls;
static unsigned led_writes;
static int spi_awake;
static uint32_t gpio_config[32];
static h2_pal_led_color_t expected_led[H2_ZERO_BK_LED_STRIP_PIXEL_COUNT];
static const h2_pal_led_api_t* led;
static uint8_t command;
static unsigned window[4];
static const void* last_pixel;
static size_t last_pixel_len;
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
int bk_pm_module_vote_sleep_ctrl(pm_sleep_module_name_e module,
                                 uint32_t sleep_state, uint32_t sleep_time) {
  assert(module == PM_SLEEP_MODULE_NAME_SPI_1 && sleep_time == 0u);
  assert(spi_awake == (sleep_state != 0u));
  spi_awake = sleep_state == 0u;
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
const h2_pal_periph_api_t* h2_bk7258_board_periph_api(void) { return NULL; }
uint32_t bk_gpio_get_value(gpio_id_t pin) {
  assert(pin < 32u);
  return gpio_config[pin];
}
int bk_gpio_set_value(gpio_id_t pin, uint32_t value) {
  assert(pin < 32u);
  gpio_config[pin] = value;
  return 0;
}
int bk_spi_init(uint32_t id, const spi_config_t* c) {
  if (id == H2_ZERO_BK_LED_STRIP_SPI_ID) {
    assert(CONFIG_SPI_DMA && c->dma_mode == SPI_DMA_MODE_ENABLE);
    assert(c->baud_rate == H2_ZERO_BK_LED_STRIP_FREQUENCY_HZ);
    gpio_config[H2_ZERO_BK_POWER_BUTTON_GPIO] = 0u;
    gpio_config[H2_ZERO_BK_USB_INSERT_GPIO] = 0u;
    gpio_config[H2_ZERO_BK_AUDIO_SELECT_GPIO] = 0u;
    return 0;
  }
  assert(id == 0 && !spi_owned);
  assert(c->dma_mode == SPI_DMA_MODE_ENABLE);
  spi_owned = 1;
  return fault(SPI_INIT);
}
int bk_spi_deinit(uint32_t id) {
  assert(id == 0);
  int rc = fault(SPI_CLOSE);
  if (!rc) spi_owned = 0;
  return rc;
}
static int write_pixels(uint32_t id, const void* data, uint32_t len) {
  assert(id == 0 && spi_owned && dc && command == 0x2c);
  int rc = fault(SPI_PIXELS);
  if (rc) return rc;
  unsigned width = window[1] - window[0] + 1,
           height = window[3] - window[2] + 1;
  assert(len == width * height * 2 && len <= 65536u &&
         window[1] < H2_ZERO_BK_LCD_WIDTH && window[3] < H2_ZERO_BK_LCD_HEIGHT);
  const uint8_t* p = data;
  for (unsigned y = 0; y < height; ++y)
    for (unsigned x = 0; x < width; ++x) {
      size_t i = ((size_t)y * width + x) * 2;
      actual[(window[2] + y) * H2_ZERO_BK_LCD_WIDTH + window[0] + x] =
          (uint16_t)((p[i] << 8) | p[i + 1]);
    }
  last_pixel = data;
  last_pixel_len = len;
  ++pixel_calls;
  return 0;
}
int bk_spi_write_bytes(uint32_t id, const void* data, uint32_t len) {
  assert(spi_awake);
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
  assert(spi_awake);
  assert(CONFIG_SPI_DMA);
  if (id == H2_ZERO_BK_LED_STRIP_SPI_ID) {
    const uint8_t* bytes = data;
    size_t first = 0u, last = len;
    while (first < len && bytes[first] == 0u) ++first;
    while (last > first && bytes[last - 1u] == 0u) --last;
    assert(first * 8u * 1000000u >= 50u * H2_ZERO_BK_LED_STRIP_FREQUENCY_HZ);
    assert((len - last) * 8u * 1000000u >=
           50u * H2_ZERO_BK_LED_STRIP_FREQUENCY_HZ);
    assert(last - first == H2_ZERO_BK_LED_STRIP_PIXEL_COUNT * 12u);
    for (size_t i = 0u; i < H2_ZERO_BK_LED_STRIP_PIXEL_COUNT; ++i) {
      const uint8_t expected[] = {expected_led[i].g, expected_led[i].r,
                                  expected_led[i].b};
      for (size_t channel = 0u; channel < 3u; ++channel) {
        uint8_t value = 0u;
        for (size_t bit = 0u; bit < 8u; ++bit) {
          const uint8_t encoded =
              bytes[first + i * 12u + channel * 4u + bit / 2u];
          const uint8_t nibble = (encoded >> (bit % 2u == 0u ? 4u : 0u)) & 15u;
          assert(nibble == 8u || nibble == 14u);
          value = (uint8_t)((value << 1u) | (nibble == 14u));
        }
        assert(value == expected[channel]);
      }
    }
    ++led_writes;
    return 0;
  }
  return write_pixels(id, data, len);
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
  assert(pixel_calls > captures && p == last_pixel && len == last_pixel_len);
  assert(rect->x == 0 && rect->y == (int)window[2] &&
         rect->width == H2_ZERO_BK_LCD_WIDTH);
  for (int y = 0; y < rect->height; ++y)
    for (int x = 0; x < rect->width; ++x) {
      size_t i = ((size_t)y * rect->width + x) * 2;
      captured[(size_t)(rect->y + y) * H2_ZERO_BK_LCD_WIDTH + x] =
          (uint16_t)((p[i] << 8) | p[i + 1]);
    }
  ++captures;
  const h2_pal_led_color_t color = {.r = 0x26u, .g = 0x91u, .b = 0x5cu};
  for (size_t i = 0u; i < H2_ZERO_BK_LED_STRIP_PIXEL_COUNT; ++i)
    expected_led[i] = color;
  const unsigned before = led_writes;
  assert(led->vtable->set_solid(led->user, H2_ZERO_BK_LED_STRIP_ID, color) ==
         H2_PAL_OK);
  memset(expected_led, 0, sizeof(expected_led));
  assert(led->vtable->clear(led->user, H2_ZERO_BK_LED_STRIP_ID) == H2_PAL_OK);
  assert(led_writes == before + 2u);
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
  led = h2_bk7258_board_led_api();
  h2_pal_led_info_t led_info;
  assert(led->vtable->get_info(led->user, H2_ZERO_BK_LED_STRIP_ID, &led_info) ==
         H2_PAL_OK);
  assert(led_info.pixel_count == H2_ZERO_BK_LED_STRIP_PIXEL_COUNT);
  gpio_config[H2_ZERO_BK_POWER_BUTTON_GPIO] = 0x31u;
  gpio_config[H2_ZERO_BK_USB_INSERT_GPIO] = 0x42u;
  gpio_config[H2_ZERO_BK_AUDIO_SELECT_GPIO] = 0x53u;
  for (size_t i = 0u; i < H2_ZERO_BK_LED_STRIP_PIXEL_COUNT; ++i)
    expected_led[i] = (h2_pal_led_color_t){.r = (uint8_t)(i + 1u),
                                           .g = (uint8_t)(i + 19u),
                                           .b = (uint8_t)(i + 37u)};
  assert(led->vtable->set_frame(led->user, H2_ZERO_BK_LED_STRIP_ID,
                                expected_led,
                                H2_ZERO_BK_LED_STRIP_PIXEL_COUNT) == H2_PAL_OK);
  assert(led_writes == 1u &&
         gpio_config[H2_ZERO_BK_POWER_BUTTON_GPIO] == 0x31u &&
         gpio_config[H2_ZERO_BK_USB_INSERT_GPIO] == 0x42u &&
         gpio_config[H2_ZERO_BK_AUDIO_SELECT_GPIO] == 0x53u);
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
  faults[SPI_PIXELS] = 1;
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
