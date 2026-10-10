#include "h2_web_main_thread.h"
#include "h2_web_platform.h"
#include <emscripten.h>
#include <emscripten/threading.h>
#include <stdio.h>
#include <string.h>
#define CHECK(test)                                                            \
  do {                                                                         \
    if (!(test)) {                                                             \
      fprintf(stderr, "web output line %d: %s\n", __LINE__, #test);            \
      emscripten_force_exit(1);                                                \
    }                                                                          \
  } while (0)

/* clang-format off */
EM_JS(void, verify_output, (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ['i32'], 'i32', step => {
    if (step === 0) { Module.canvas = {style: {}}; return 1; }
    const out = Module.h2WebOutputs, led = out?.leds?.[77];
    if (step === 1) return led?.name === 'chest' && led.pixels.length === 2 && led.pixels.every(p => p.r===10 && p.g===20 && p.b===30 && p.w===40);
    if (step === 2) return led.brightnessPercent === 25 && led.pixels[0].r === 10;
    if (step === 3) return led.pixels[0].r===100 && led.pixels[1].b===255 && led.brightnessPercent===25;
    if (step === 4) return led.pixels.every(p => p.r+p.g+p.b+p.w===0);
    if (step === 5) return out.display.brightnessPercent === 37 && Module.canvas.style.filter === 'brightness(37%)';
    if (step === 6) return out.display.brightnessPercent === 0;
    if (step === 7) return out.speakerVolumePercent === 42;
    return 0;
  });
});
/* clang-format on */
static void verify(int step) {
  CHECK(h2_web_main_call(verify_output, (const void *[]){&step}).i32 == 1);
}
static h2_pal_result_t get_periph(void *user, h2_pal_periph_id_t id,
                                  h2_pal_periph_info_t *out_info) {
  (void)user;
  static const h2_pal_periph_led_strip_payload_t payload = {
      .led_count = 2, .channels_per_led = 4};
  *out_info = (h2_pal_periph_info_t){0};
  if (id != 77)
    return H2_PAL_ERR_NOT_FOUND;
  *out_info = (h2_pal_periph_info_t){.id = 77,
                                     .type = H2_PAL_PERIPH_TYPE_LED_STRIP,
                                     .payload = &payload,
                                     .payload_size = sizeof(payload)};
  memcpy(out_info->name, "chest", 6);
  return H2_PAL_OK;
}
int main(void) {
  verify(0);
  h2_web_platform_t *platform =
      h2_web_platform_create(&(h2_web_platform_config_t){1, 1});
  CHECK(platform);
  static const h2_pal_periph_vtable_t vtable = {.get = get_periph};
  const h2_pal_periph_api_t periph = {NULL, &vtable};
  CHECK(h2_web_platform_configure_leds(platform, &periph) == H2_PAL_OK);
  CHECK(h2_web_platform_configure_leds(platform, &periph) ==
        H2_PAL_ERR_INVALID_STATE);
  const h2_pal_led_api_t *led = h2_web_platform_led_api(platform);
  h2_pal_led_info_t info;
  CHECK(h2_pal_led_get_info(led, 77, &info) == H2_PAL_OK &&
        info.pixel_count == 2 && info.channels_per_pixel == 4);
  CHECK(h2_pal_led_get_info(led, 78, &info) == H2_PAL_ERR_NOT_FOUND);
  CHECK(h2_pal_led_set_solid(led, 77, (h2_pal_led_color_t){10, 20, 30, 40}) ==
        H2_PAL_OK);
  verify(1);
  CHECK(h2_pal_led_set_brightness_percent(led, 77, 25) == H2_PAL_OK);
  verify(2);
  const h2_pal_led_color_t pixels[] = {{100, 0, 0, 0}, {0, 0, 255, 0}};
  CHECK(h2_pal_led_set_frame(led, 77, pixels, 1) == H2_PAL_ERR_INVALID_ARG);
  verify(2);
  CHECK(h2_pal_led_set_frame(led, 77, pixels, 2) == H2_PAL_OK);
  verify(3);
  CHECK(h2_pal_led_clear(led, 77) == H2_PAL_OK);
  verify(4);
  const h2_pal_display_api_t *display = h2_web_platform_display_api(platform);
  CHECK(h2_pal_display_open(display) == H2_PAL_OK);
  CHECK(h2_pal_display_set_brightness_percent(display, 37) == H2_PAL_OK);
  verify(5);
  CHECK(h2_pal_display_set_brightness_percent(display, 101) ==
        H2_PAL_ERR_INVALID_ARG);
  verify(5);
  CHECK(h2_pal_display_set_brightness_percent(display, 0) == H2_PAL_OK);
  verify(6);
  CHECK(h2_pal_display_close(display) == H2_PAL_OK);
  const h2_pal_audio_api_t *audio = h2_web_platform_audio_api(platform);
  CHECK(h2_pal_audio_set_speaker_volume_percent(audio, 42) == H2_PAL_OK);
  verify(7);
  h2_pal_result_t rc = H2_PAL_ERR_BUSY;
  for (unsigned i = 0; i < 1000 && rc == H2_PAL_ERR_BUSY; ++i) {
    rc = h2_web_platform_destroy(platform);
    if (rc == H2_PAL_ERR_BUSY)
      emscripten_thread_sleep(1);
  }
  CHECK(rc == H2_PAL_OK);
  puts("H2_WEB_OUTPUTS PASS");
  emscripten_force_exit(0);
  return 0;
}
