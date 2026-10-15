#include "h2_web_platform_internal.h"

#include <emscripten.h>
#include <string.h>

/* The board owns LED identities/counts. The Web provider only exports actual
 * PAL output, never a product effect or independent animation. */
/* clang-format off */
EM_JS(void, h2_web_led_output_js,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["u32", "pointer", "pointer", "u32", "i32", "u32"], null,
    (id, namePointer, bytes, count, operation, value) => {
      const name = UTF8ToString(namePointer);
      const output = Module.h2WebOutputs ||= {version: 1};
      const leds = output.leds ||= Object.create(null);
      const previous = leds[id] || {id, name, brightnessPercent: 100,
        pixels: Array.from({length: count}, () => ({r: 0, g: 0, b: 0, w: 0}))};
      let pixels = previous.pixels;
      if (operation === 0) pixels = Array.from({length: count}, (_, i) => {
        const p = bytes + i * 4;
        return {r: HEAPU8[p], g: HEAPU8[p+1], b: HEAPU8[p+2], w: HEAPU8[p+3]};
      });
      else if (operation === 1) pixels = Array.from({length: count}, () =>
        ({r: value & 255, g: (value >>> 8) & 255, b: (value >>> 16) & 255, w: value >>> 24}));
      leds[id] = {...previous, pixels,
        brightnessPercent: operation === 2 ? value : previous.brightnessPercent};
    });
});
/* clang-format on */

static h2_pal_result_t led_descriptor(h2_web_platform_t *platform,
                                      h2_pal_led_id_t id,
                                      h2_pal_periph_info_t *out_periph,
                                      h2_pal_led_info_t *out_info) {
  *out_info = (h2_pal_led_info_t){0};
  if (!platform->led_periph.vtable)
    return H2_PAL_ERR_UNSUPPORTED;
  h2_pal_result_t rc = h2_pal_periph_get(&platform->led_periph, id, out_periph);
  if (rc != H2_PAL_OK)
    return rc;
  if (out_periph->type != H2_PAL_PERIPH_TYPE_LED_STRIP)
    return H2_PAL_ERR_NOT_FOUND;
  if (!out_periph->payload ||
      out_periph->payload_size != sizeof(h2_pal_periph_led_strip_payload_t) ||
      !memchr(out_periph->name, 0, sizeof(out_periph->name)))
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_periph_led_strip_payload_t strip;
  memcpy(&strip, out_periph->payload, sizeof(strip));
  if (!strip.led_count || strip.led_count > 256 ||
      (strip.channels_per_led != 3 && strip.channels_per_led != 4))
    return H2_PAL_ERR_UNSUPPORTED;
  *out_info = (h2_pal_led_info_t){.id = id,
                                  .pixel_count = strip.led_count,
                                  .channels_per_pixel = strip.channels_per_led,
                                  .supports_brightness = 1,
                                  .frame_commit_is_atomic = 1};
  return H2_PAL_OK;
}
static h2_pal_result_t led_info(void *user, h2_pal_led_id_t id,
                                h2_pal_led_info_t *out_info) {
  H2_WEB_STATE_GUARD();
  if (!out_info)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_periph_info_t periph = {0};
  return led_descriptor(user, id, &periph, out_info);
}
static h2_pal_result_t led_write(void *user, h2_pal_led_id_t id,
                                 const h2_pal_led_color_t *pixels, size_t count,
                                 int operation, uint32_t value) {
  H2_WEB_STATE_GUARD();
  h2_pal_periph_info_t periph = {0};
  h2_pal_led_info_t info;
  h2_pal_result_t rc = led_descriptor(user, id, &periph, &info);
  if (rc != H2_PAL_OK)
    return rc;
  if (operation == 0 && (!pixels || count != info.pixel_count))
    return H2_PAL_ERR_INVALID_ARG;
  if (operation == 2 && value > 100)
    return H2_PAL_ERR_INVALID_ARG;
  /* Explicit RGBA bytes avoid coupling JavaScript to C struct layout. */
  uint8_t bytes[256 * 4];
  if (operation == 0)
    for (size_t i = 0; i < count; ++i) {
      if (info.channels_per_pixel == 3 && pixels[i].w)
        return H2_PAL_ERR_UNSUPPORTED;
      bytes[i * 4] = pixels[i].r;
      bytes[i * 4 + 1] = pixels[i].g;
      bytes[i * 4 + 2] = pixels[i].b;
      bytes[i * 4 + 3] = pixels[i].w;
    }
  if (operation == 1 && info.channels_per_pixel == 3 && (value >> 24))
    return H2_PAL_ERR_UNSUPPORTED;
  const char *name = periph.name;
  const uint8_t *data = bytes;
  uint32_t length = info.pixel_count;
  (void)h2_web_main_call(
      h2_web_led_output_js,
      (const void *[]){&id, &name, &data, &length, &operation, &value});
  return H2_PAL_OK;
}
static h2_pal_result_t led_frame(void *user, h2_pal_led_id_t id,
                                 const h2_pal_led_color_t *pixels,
                                 size_t count) {
  return led_write(user, id, pixels, count, 0, 0);
}
static h2_pal_result_t led_solid(void *user, h2_pal_led_id_t id,
                                 h2_pal_led_color_t color) {
  const uint32_t value = color.r | ((uint32_t)color.g << 8) |
                         ((uint32_t)color.b << 16) | ((uint32_t)color.w << 24);
  return led_write(user, id, NULL, 0, 1, value);
}
static h2_pal_result_t led_clear(void *user, h2_pal_led_id_t id) {
  return led_solid(user, id, (h2_pal_led_color_t){0});
}
static h2_pal_result_t led_brightness(void *user, h2_pal_led_id_t id,
                                      uint8_t percent) {
  return led_write(user, id, NULL, 0, 2, percent);
}
h2_pal_result_t
h2_web_platform_configure_leds(h2_web_platform_t *platform,
                               const h2_pal_periph_api_t *periph) {
  H2_WEB_STATE_GUARD();
  if (!platform || !periph)
    return H2_PAL_ERR_INVALID_ARG;
  if (platform->led_api.vtable)
    return H2_PAL_ERR_INVALID_STATE;
  static const h2_pal_led_vtable_t vtable = {.get_info = led_info,
                                             .set_frame = led_frame,
                                             .set_solid = led_solid,
                                             .clear = led_clear,
                                             .set_brightness_percent =
                                                 led_brightness};
  platform->led_periph = *periph;
  platform->led_api = (h2_pal_led_api_t){platform, &vtable};
  return H2_PAL_OK;
}
const h2_pal_led_api_t *h2_web_platform_led_api(h2_web_platform_t *platform) {
  return platform ? &platform->led_api : NULL;
}
