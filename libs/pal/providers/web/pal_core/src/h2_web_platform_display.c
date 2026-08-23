#include "h2_web_platform_internal.h"

#include <emscripten.h>
#include <stdlib.h>
#include <string.h>

EM_JS(int, h2_web_present_rgba,
      (const uint32_t *pixels, int width, int height), {
        const canvas = Module['canvas'];
        if (!canvas) return 0;
        const context = canvas.getContext('2d', {alpha: false});
        if (!context) return 0;
        const source = new Uint8ClampedArray(HEAPU8.buffer, pixels,
                                             width * height * 4);
        const image = new ImageData(new Uint8ClampedArray(source), width,
                                    height);
        context.putImageData(image, 0, 0);
        return 1;
      });

EM_JS(int, h2_web_install_pointer_js,
      (uintptr_t address, int width, int height), {
        const canvas = Module['canvas'];
        const cleanups = Module['h2WebPlatformPointerCleanups'] ||= new Map();
        if (!canvas) return 0;
        if (cleanups.has(address)) return 1;
        const update = (event, pressed) => {
          event.preventDefault();
          const rect = canvas.getBoundingClientRect();
          const x = Math.max(0, Math.min(width - 1,
              Math.floor((event.clientX - rect.left) * width / rect.width)));
          const y = Math.max(0, Math.min(height - 1,
              Math.floor((event.clientY - rect.top) * height / rect.height)));
          Module['_h2_web_platform_pointer'](address, x, y, pressed ? 1 : 0);
        };
        const down = event => {
          canvas.setPointerCapture(event.pointerId);
          update(event, true);
        };
        const move = event => update(event, event.buttons !== 0);
        const up = event => update(event, false);
        canvas.addEventListener('pointerdown', down);
        canvas.addEventListener('pointermove', move);
        canvas.addEventListener('pointerup', up);
        canvas.addEventListener('pointercancel', up);
        cleanups.set(address, () => {
          canvas.removeEventListener('pointerdown', down);
          canvas.removeEventListener('pointermove', move);
          canvas.removeEventListener('pointerup', up);
          canvas.removeEventListener('pointercancel', up);
        });
        return 1;
      });

EM_JS(void, h2_web_remove_pointer_js, (uintptr_t address), {
  const cleanups = Module['h2WebPlatformPointerCleanups'];
  const cleanup = cleanups && cleanups.get(address);
  if (cleanup) {
    cleanup();
    cleanups.delete(address);
  }
});

EM_JS(int, h2_web_set_brightness_js, (uint32_t percent), {
  const canvas = Module['canvas'];
  if (!canvas) return 0;
  canvas.style.filter = `brightness(${percent}%)`;
  return 1;
});

EM_JS(int, h2_web_canvas_available_js, (), {
  return Module['canvas'] ? 1 : 0;
});

static void h2_web_touch_push(h2_web_platform_t *platform,
                              h2_pal_touch_event_kind_t kind, int32_t x,
                              int32_t y) {
  if (!platform->touch_opened) {
    return;
  }
  if (kind == H2_PAL_TOUCH_EVENT_MOVE && platform->touch_count != 0u) {
    const size_t last = (platform->touch_head + platform->touch_count - 1u) %
                        H2_WEB_TOUCH_EVENT_CAPACITY;
    if (platform->touch_events[last].kind == H2_PAL_TOUCH_EVENT_MOVE) {
      platform->touch_events[last].x = x;
      platform->touch_events[last].y = y;
      return;
    }
  }
  if (platform->touch_count == H2_WEB_TOUCH_EVENT_CAPACITY) {
    platform->touch_head =
        (platform->touch_head + 1u) % H2_WEB_TOUCH_EVENT_CAPACITY;
    --platform->touch_count;
  }
  const size_t tail = (platform->touch_head + platform->touch_count) %
                      H2_WEB_TOUCH_EVENT_CAPACITY;
  platform->touch_events[tail] = (h2_pal_touch_event_t){
      .kind = kind,
      .x = x,
      .y = y,
  };
  ++platform->touch_count;
}

EMSCRIPTEN_KEEPALIVE void h2_web_platform_pointer(uintptr_t address, int32_t x,
                                                  int32_t y, int pressed) {
  h2_web_platform_t *platform = (h2_web_platform_t *)address;
  if (platform == NULL || platform->shutting_down) {
    return;
  }
  h2_pal_touch_event_kind_t kind = H2_PAL_TOUCH_EVENT_MOVE;
  if (!platform->pointer_pressed && pressed) {
    kind = H2_PAL_TOUCH_EVENT_DOWN;
  } else if (platform->pointer_pressed && !pressed) {
    kind = H2_PAL_TOUCH_EVENT_UP;
  }
  platform->pointer_x = x;
  platform->pointer_y = y;
  platform->pointer_pressed = pressed;
  h2_web_touch_push(platform, kind, x, y);
}

void h2_web_platform_install_pointer(h2_web_platform_t *platform) {
  if (platform != NULL && !platform->pointer_installed) {
    platform->pointer_installed = h2_web_install_pointer_js(
        (uintptr_t)platform, platform->width, platform->height) != 0;
  }
}

h2_pal_result_t h2_web_platform_read_pointer(void *user, int32_t *out_x,
                                             int32_t *out_y,
                                             int *out_pressed) {
  h2_web_platform_t *platform = user;
  if (platform == NULL || out_x == NULL || out_y == NULL ||
      out_pressed == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_x = platform->pointer_x;
  *out_y = platform->pointer_y;
  *out_pressed = platform->pointer_pressed;
  return H2_PAL_OK;
}

static h2_pal_result_t h2_web_touch_open(void *user) {
  h2_web_platform_t *platform = user;
  if (platform == NULL || platform->shutting_down) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  platform->touch_head = 0u;
  platform->touch_count = 0u;
  platform->touch_opened = true;
  h2_web_platform_install_pointer(platform);
  if (!platform->pointer_installed) {
    platform->touch_opened = false;
    return H2_PAL_ERR_UNAVAILABLE;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t h2_web_touch_get_info(void *user,
                                             h2_pal_touch_info_t *out_info) {
  h2_web_platform_t *platform = user;
  if (out_info != NULL) *out_info = (h2_pal_touch_info_t){0};
  if (platform == NULL || out_info == NULL || !platform->touch_opened) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  *out_info = (h2_pal_touch_info_t){
      .width = (uint32_t)platform->width,
      .height = (uint32_t)platform->height,
  };
  return H2_PAL_OK;
}

static h2_pal_result_t h2_web_touch_poll(void *user,
                                         h2_pal_touch_event_t *out_event) {
  h2_web_platform_t *platform = user;
  if (out_event != NULL) *out_event = (h2_pal_touch_event_t){0};
  if (platform == NULL || out_event == NULL || !platform->touch_opened) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (platform->touch_count == 0u) {
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  *out_event = platform->touch_events[platform->touch_head];
  platform->touch_head =
      (platform->touch_head + 1u) % H2_WEB_TOUCH_EVENT_CAPACITY;
  --platform->touch_count;
  return H2_PAL_OK;
}

static h2_pal_result_t h2_web_touch_close(void *user) {
  h2_web_platform_t *platform = user;
  if (platform == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  platform->touch_opened = false;
  platform->touch_head = 0u;
  platform->touch_count = 0u;
  if (platform->pointer_installed) {
    h2_web_remove_pointer_js((uintptr_t)platform);
    platform->pointer_installed = false;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t h2_web_display_open(void *user) {
  h2_web_platform_t *platform = user;
  if (platform == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (!h2_web_canvas_available_js()) {
    return H2_PAL_ERR_UNAVAILABLE;
  }
  if (platform->rgba == NULL) {
    platform->rgba = calloc((size_t)platform->width * (size_t)platform->height,
                            sizeof(*platform->rgba));
  }
  return platform->rgba == NULL ? H2_PAL_ERR_NO_MEMORY : H2_PAL_OK;
}

static h2_pal_result_t h2_web_display_info(void *user,
                                           h2_display_info_t *out_info) {
  h2_web_platform_t *platform = user;
  if (out_info != NULL) *out_info = (h2_display_info_t){0};
  if (platform == NULL || out_info == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_info = (h2_display_info_t){
      .width = platform->width,
      .height = platform->height,
      .native_format = H2_DISPLAY_PIXEL_RGB565,
  };
  return H2_PAL_OK;
}

static h2_pal_result_t h2_web_display_draw(
    void *user, const h2_display_rect_t *rect, const void *pixels,
    size_t stride_bytes, h2_display_pixel_format_t format) {
  h2_web_platform_t *platform = user;
  if (platform == NULL || rect == NULL || pixels == NULL ||
      platform->rgba == NULL || format != H2_DISPLAY_PIXEL_RGB565 ||
      rect->x < 0 || rect->y < 0 || rect->width <= 0 || rect->height <= 0 ||
      rect->width > platform->width - rect->x ||
      rect->height > platform->height - rect->y ||
      (size_t)rect->width > SIZE_MAX / sizeof(uint16_t) ||
      stride_bytes < (size_t)rect->width * sizeof(uint16_t) ||
      ((size_t)rect->height - 1u) > SIZE_MAX / stride_bytes) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  for (int32_t y = 0; y < rect->height; ++y) {
    const uint8_t *source =
        (const uint8_t *)pixels + (size_t)y * stride_bytes;
    uint32_t *destination = platform->rgba +
        (size_t)(rect->y + y) * (size_t)platform->width + (size_t)rect->x;
    for (int32_t x = 0; x < rect->width; ++x) {
      uint16_t pixel;
      memcpy(&pixel, source + (size_t)x * sizeof(pixel), sizeof(pixel));
      const uint32_t red = ((pixel >> 11u) & 0x1fu) * 255u / 31u;
      const uint32_t green = ((pixel >> 5u) & 0x3fu) * 255u / 63u;
      const uint32_t blue = (pixel & 0x1fu) * 255u / 31u;
      destination[x] = red | (green << 8u) | (blue << 16u) | 0xff000000u;
    }
  }
  return H2_PAL_OK;
}

static h2_pal_result_t h2_web_display_present(void *user) {
  h2_web_platform_t *platform = user;
  if (platform == NULL || platform->rgba == NULL) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  return h2_web_present_rgba(platform->rgba, platform->width,
                             platform->height)
             ? H2_PAL_OK
             : H2_PAL_ERR_UNAVAILABLE;
}

static h2_pal_result_t h2_web_display_brightness(void *user,
                                                 uint32_t percent) {
  if (user == NULL || percent > 100u) return H2_PAL_ERR_INVALID_ARG;
  return h2_web_set_brightness_js(percent) ? H2_PAL_OK
                                           : H2_PAL_ERR_UNAVAILABLE;
}

static h2_pal_result_t h2_web_display_close(void *user) {
  h2_web_platform_t *platform = user;
  if (platform == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  free(platform->rgba);
  platform->rgba = NULL;
  return H2_PAL_OK;
}

void h2_web_platform_display_init(h2_web_platform_t *platform) {
  static const h2_pal_display_vtable_t display_vtable = {
      .open = h2_web_display_open,
      .get_info = h2_web_display_info,
      .draw_bitmap = h2_web_display_draw,
      .present = h2_web_display_present,
      .set_brightness_percent = h2_web_display_brightness,
      .close = h2_web_display_close,
  };
  static const h2_pal_touch_vtable_t touch_vtable = {
      .open = h2_web_touch_open,
      .get_info = h2_web_touch_get_info,
      .poll_event = h2_web_touch_poll,
      .close = h2_web_touch_close,
  };
  platform->display_api = (h2_pal_display_api_t){
      .user = platform,
      .vtable = &display_vtable,
  };
  platform->touch_api = (h2_pal_touch_api_t){
      .user = platform,
      .vtable = &touch_vtable,
  };
}

void h2_web_platform_display_deinit(h2_web_platform_t *platform) {
  h2_web_remove_pointer_js((uintptr_t)platform);
  platform->pointer_installed = false;
  (void)h2_web_touch_close(platform);
  (void)h2_web_display_close(platform);
}
