#include "h2_esp_board_private.h"

#include "led_strip.h"

#include <assert.h>
#include <string.h>

struct h2_test_led_strip {
  h2_pal_led_color_t pixels[H2_TIGA_ESP_LED_STRIP_PIXEL_COUNT];
};

static struct h2_test_led_strip strip;
static unsigned creates;
static unsigned refreshes;
static esp_err_t write_result;

const h2_pal_periph_api_t* h2_esp_board_periph_api(void) { return NULL; }

esp_err_t led_strip_new_rmt_device(const led_strip_config_t* config,
                                   const led_strip_rmt_config_t* rmt,
                                   led_strip_handle_t* out) {
  assert(config->strip_gpio_num == 8);
  assert(config->max_leds == H2_TIGA_ESP_LED_STRIP_PIXEL_COUNT);
  assert(config->led_model == LED_MODEL_WS2812);
  assert(config->color_component_format == LED_STRIP_COLOR_COMPONENT_FMT_GRB);
  assert(rmt->flags.with_dma);
  /* The SDK rounds nanoseconds up to RMT ticks. WS2812's 300 ns zero
   * pulse must stay below 400 ns, rather than being read as a one bit. */
  const uint64_t zero_ticks =
      (300ull * rmt->resolution_hz + 999999999ull) / 1000000000ull;
  assert(zero_ticks * 1000000000ull < 400ull * rmt->resolution_hz);
  /* All model timings must be integral ticks for floor/ceil encoders. */
  const unsigned timings_ns[] = {300u, 600u, 900u};
  for (size_t i = 0u; i < sizeof(timings_ns) / sizeof(timings_ns[0]); ++i)
    assert((uint64_t)timings_ns[i] * rmt->resolution_hz % 1000000000ull == 0u);
  ++creates;
  *out = &strip;
  return ESP_OK;
}

esp_err_t led_strip_set_pixel(led_strip_handle_t handle, uint32_t index,
                              uint32_t red, uint32_t green, uint32_t blue) {
  assert(handle == &strip && index < H2_TIGA_ESP_LED_STRIP_PIXEL_COUNT);
  if (write_result != ESP_OK) return write_result;
  strip.pixels[index] = (h2_pal_led_color_t){
      .r = (uint8_t)red,
      .g = (uint8_t)green,
      .b = (uint8_t)blue,
  };
  return ESP_OK;
}

esp_err_t led_strip_refresh(led_strip_handle_t handle) {
  assert(handle == &strip);
  ++refreshes;
  return ESP_OK;
}

esp_err_t led_strip_clear(led_strip_handle_t handle) {
  assert(handle == &strip);
  memset(strip.pixels, 0, sizeof(strip.pixels));
  return led_strip_refresh(handle);
}

static void expect_solid(uint8_t red, uint8_t green, uint8_t blue) {
  for (size_t i = 0u; i < H2_TIGA_ESP_LED_STRIP_PIXEL_COUNT; ++i) {
    assert(strip.pixels[i].r == red);
    assert(strip.pixels[i].g == green);
    assert(strip.pixels[i].b == blue);
  }
}

int main(void) {
  const h2_pal_led_api_t* api = h2_esp_board_led_api();
  const h2_pal_led_id_t id = H2_TIGA_ESP_LED_STRIP_ID;
  assert(h2_esp_board_led_prepare_sleep() == H2_PAL_OK);
  assert(creates == 0u && refreshes == 0u);
  const h2_pal_led_color_t blue = {.g = 80u, .b = 255u};
  assert(h2_pal_led_set_solid(api, id, blue) == H2_PAL_OK);
  expect_solid(0u, 80u, 255u);
  const h2_pal_led_color_t red = {.r = 255u};
  assert(h2_pal_led_set_solid(api, id, red) == H2_PAL_OK);
  expect_solid(255u, 0u, 0u);
  assert(h2_pal_led_set_brightness_percent(api, id, 50u) == H2_PAL_OK);
  assert(h2_pal_led_set_solid(api, id, blue) == H2_PAL_OK);
  expect_solid(0u, 40u, 127u);
  assert(h2_pal_led_set_brightness_percent(api, id, 0u) == H2_PAL_OK);
  assert(h2_pal_led_set_solid(api, id, blue) == H2_PAL_OK);
  expect_solid(0u, 0u, 0u);
  assert(h2_pal_led_set_brightness_percent(api, id, 100u) == H2_PAL_OK);
  h2_pal_led_color_t frame[H2_TIGA_ESP_LED_STRIP_PIXEL_COUNT] = {0};
  frame[0] = blue;
  frame[1] = red;
  assert(h2_pal_led_set_frame(api, id, frame,
                              H2_TIGA_ESP_LED_STRIP_PIXEL_COUNT) == H2_PAL_OK);
  assert(strip.pixels[0].r == 0u && strip.pixels[0].g == 80u &&
         strip.pixels[0].b == 255u);
  assert(strip.pixels[1].r == 255u && strip.pixels[1].g == 0u &&
         strip.pixels[1].b == 0u);
  const unsigned before_error = refreshes;
  write_result = ESP_ERR_INVALID_STATE;
  assert(h2_pal_led_set_solid(api, id, blue) == H2_PAL_ERR_INVALID_STATE);
  assert(refreshes == before_error);
  write_result = ESP_OK;
  assert(h2_esp_board_led_prepare_sleep() == H2_PAL_OK);
  expect_solid(0u, 0u, 0u);
  assert(creates == 1u);
  return 0;
}
