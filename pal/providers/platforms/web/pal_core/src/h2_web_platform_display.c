#include "h2_web_main_thread.h"
#include "h2_web_platform_internal.h"

#include <emscripten.h>
#include <stdlib.h>
#include <string.h>

/* clang-format off */
EM_JS(void, h2_web_present_rgba,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["pointer", "i32", "i32"], "i32",
    (pixels, width, height) => {
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
});
/* clang-format on */

/* clang-format off */
EM_JS(void, h2_web_install_pointer_js,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["u32", "i32", "i32", "i32"], "i32",
    (address, width, height, multiTouch) => {
        const canvas = Module['canvas'];
        const cleanups = Module['h2WebPlatformPointerCleanups'] ||= new Map();
        if (!canvas) return 0;
        if (cleanups.has(address)) return 1;
        const contacts = new Map();
        const Down = 1, Move = 2, Up = 3, Cancel = 4;
        const send = (id, contact, kind) => {
          Module['_h2_web_platform_pointer'](address, id, contact.x, contact.y,
                                            kind, contact.primary ? 1 : 0);
        };
        const update = (event, contact, kind) => {
          event.preventDefault();
          const rect = canvas.getBoundingClientRect();
          if (rect.width > 0 && rect.height > 0) {
            contact.x = Math.max(0, Math.min(width - 1,
                Math.floor((event.clientX - rect.left) * width / rect.width)));
            contact.y = Math.max(0, Math.min(height - 1,
                Math.floor((event.clientY - rect.top) * height / rect.height)));
          }
          send(event.pointerId, contact, kind);
        };
        const down = event => {
          if (event.button !== 0 || contacts.has(event.pointerId) ||
              (!multiTouch && (!event.isPrimary || contacts.size))) return;
          const contact = {x: 0, y: 0, primary: contacts.size === 0};
          contacts.set(event.pointerId, contact);
          canvas.setPointerCapture(event.pointerId);
          update(event, contact, Down);
        };
        const move = event => {
          const contact = contacts.get(event.pointerId);
          if (contact) update(event, contact, Move);
          else if (event.isPrimary && !event.buttons && !contacts.size)
            update(event, {x: 0, y: 0, primary: true}, Move);
        };
        const up = event => {
          const contact = contacts.get(event.pointerId);
          if (!contact) return;
          contacts.delete(event.pointerId);
          update(event, contact, Up);
        };
        const cancel = event => {
          const contact = contacts.get(event.pointerId);
          if (!contact) return;
          contacts.delete(event.pointerId);
          send(event.pointerId, contact, Cancel);
        };
        const cancelAll = () => {
          for (const [id, contact] of contacts) send(id, contact, Cancel);
          contacts.clear();
        };
        const visibility = () => { if (document.hidden) cancelAll(); };
        const handlers = {pointerdown: down, pointermove: move, pointerup: up,
                          pointercancel: cancel, lostpointercapture: cancel};
        for (const [type, listener] of Object.entries(handlers))
          canvas.addEventListener(type, listener);
        window.addEventListener('blur', cancelAll);
        document.addEventListener('visibilitychange', visibility);
        cleanups.set(address, () => {
          for (const [type, listener] of Object.entries(handlers))
            canvas.removeEventListener(type, listener);
          window.removeEventListener('blur', cancelAll);
          document.removeEventListener('visibilitychange', visibility);
        });
        return 1;
      });
});
/* clang-format on */

/* clang-format off */
EM_JS(void, h2_web_remove_pointer_js,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["u32"], null,
    (address) => {
  const cleanups = Module['h2WebPlatformPointerCleanups'];
  const cleanup = cleanups && cleanups.get(address);
  if (cleanup) {
    cleanup();
    cleanups.delete(address);
  }
});
});
/* clang-format on */

/* clang-format off */
EM_JS(void, h2_web_set_brightness_js,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["u32"], "i32",
    (percent) => {
  const canvas = Module['canvas'];
  if (!canvas) return 0;
  canvas.style.filter = `brightness(${percent}%)`;
  const output = Module.h2WebOutputs ||= {version: 1};
  output.display = {brightnessPercent: percent};
  return 1;
});
});
/* clang-format on */

/* clang-format off */
EM_JS(void, h2_web_canvas_available_js,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["i32", "i32"], "i32",
    (width, height) => {
  const canvas = Module['canvas'];
  if (!canvas) return 0;
  if (canvas.width !== width) canvas.width = width;
  if (canvas.height !== height) canvas.height = height;
  return 1;
});
});
/* clang-format on */

static void h2_web_touch_push(h2_web_platform_t* platform,
                              h2_pal_touch_event_t event) {
  if (!platform->touch_opened) return;
  if (event.kind == H2_PAL_TOUCH_EVENT_MOVE && platform->touch_count != 0u) {
    const size_t last = (platform->touch_head + platform->touch_count - 1u) %
                        H2_WEB_TOUCH_EVENT_CAPACITY;
    if (platform->touch_events[last].kind == H2_PAL_TOUCH_EVENT_MOVE &&
        platform->touch_events[last].pointer_id == event.pointer_id) {
      platform->touch_events[last] = event;
      return;
    }
  }
  if (platform->touch_count == H2_WEB_TOUCH_EVENT_CAPACITY) {
    if (platform->multi_touch) {
      /* A lost release must not leave either control captured. */
      platform->touch_head = 0;
      platform->touch_count = 0;
      event = (h2_pal_touch_event_t){.kind = H2_PAL_TOUCH_EVENT_CANCEL,
                                     .pointer_id = H2_PAL_TOUCH_ALL_POINTERS};
    } else {
      platform->touch_head =
          (platform->touch_head + 1u) % H2_WEB_TOUCH_EVENT_CAPACITY;
      --platform->touch_count;
    }
  }
  const size_t tail = (platform->touch_head + platform->touch_count) %
                      H2_WEB_TOUCH_EVENT_CAPACITY;
  platform->touch_events[tail] = event;
  ++platform->touch_count;
}

EMSCRIPTEN_KEEPALIVE void h2_web_platform_pointer(uintptr_t address,
                                                  uint32_t id, int32_t x,
                                                  int32_t y, int kind,
                                                  int primary) {
  H2_WEB_STATE_GUARD();
  h2_web_platform_t* platform = (h2_web_platform_t*)address;
  if (platform == NULL || platform->shutting_down) return;
  if (primary) {
    platform->pointer_x = x;
    platform->pointer_y = y;
    if (kind != H2_PAL_TOUCH_EVENT_MOVE)
      platform->pointer_pressed = kind == H2_PAL_TOUCH_EVENT_DOWN;
  }
  if (!platform->multi_touch && kind == H2_PAL_TOUCH_EVENT_CANCEL)
    kind = H2_PAL_TOUCH_EVENT_UP;
  h2_web_touch_push(
      platform,
      (h2_pal_touch_event_t){.kind = (h2_pal_touch_event_kind_t)kind,
                             .x = x,
                             .y = y,
                             .pointer_id = platform->multi_touch ? id : 0});
}

h2_pal_result_t h2_web_platform_enable_multi_touch(
    h2_web_platform_t* platform) {
  H2_WEB_STATE_GUARD();
  if (!platform || platform->shutting_down || platform->pointer_installed)
    return H2_PAL_ERR_INVALID_STATE;
  platform->multi_touch = true;
  return H2_PAL_OK;
}

void h2_web_platform_install_pointer(h2_web_platform_t* platform) {
  H2_WEB_STATE_GUARD();
  if (platform != NULL && !platform->pointer_installed) {
    platform->pointer_installed =
        ((int)h2_web_main_call(
             h2_web_install_pointer_js,
             (const void*[]){&(uintptr_t){(uintptr_t)platform},
                             &(int){platform->width}, &(int){platform->height},
                             &(int){platform->multi_touch}})
             .i32) != 0;
  }
}

h2_pal_result_t h2_web_platform_read_pointer(void* user, int32_t* out_x,
                                             int32_t* out_y, int* out_pressed) {
  H2_WEB_STATE_GUARD();
  h2_web_platform_t* platform = user;
  if (platform == NULL || out_x == NULL || out_y == NULL ||
      out_pressed == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_x = platform->pointer_x;
  *out_y = platform->pointer_y;
  *out_pressed = platform->pointer_pressed;
  return H2_PAL_OK;
}

static h2_pal_result_t h2_web_touch_open(void* user) {
  H2_WEB_STATE_GUARD();
  h2_web_platform_t* platform = user;
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

static h2_pal_result_t h2_web_touch_get_info(void* user,
                                             h2_pal_touch_info_t* out_info) {
  H2_WEB_STATE_GUARD();
  h2_web_platform_t* platform = user;
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

static h2_pal_result_t h2_web_touch_poll(void* user,
                                         h2_pal_touch_event_t* out_event) {
  H2_WEB_STATE_GUARD();
  h2_web_platform_t* platform = user;
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

static h2_pal_result_t h2_web_touch_close(void* user) {
  H2_WEB_STATE_GUARD();
  h2_web_platform_t* platform = user;
  if (platform == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  platform->touch_opened = false;
  platform->touch_head = 0u;
  platform->touch_count = 0u;
  if (platform->pointer_installed) {
    (void)h2_web_main_call(h2_web_remove_pointer_js,
                           (const void*[]){&(uintptr_t){(uintptr_t)platform}});
    platform->pointer_installed = false;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t h2_web_display_open(void* user) {
  H2_WEB_STATE_GUARD();
  h2_web_platform_t* platform = user;
  if (platform == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (!((int)h2_web_main_call(
            h2_web_canvas_available_js,
            (const void*[]){&platform->width, &platform->height})
            .i32)) {
    return H2_PAL_ERR_UNAVAILABLE;
  }
  if (platform->rgba == NULL) {
    platform->rgba = calloc((size_t)platform->width * (size_t)platform->height,
                            sizeof(*platform->rgba));
  }
  return platform->rgba == NULL ? H2_PAL_ERR_NO_MEMORY : H2_PAL_OK;
}

static h2_pal_result_t h2_web_display_info(void* user,
                                           h2_display_info_t* out_info) {
  H2_WEB_STATE_GUARD();
  h2_web_platform_t* platform = user;
  if (out_info != NULL) *out_info = (h2_display_info_t){0};
  if (platform == NULL || out_info == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  // The contract requires open first; a closed display reports that state so
  // callers such as "open if get_info fails" reopen it.
  if (platform->rgba == NULL) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  *out_info = (h2_display_info_t){
      .width = platform->width,
      .height = platform->height,
      .native_format = H2_DISPLAY_PIXEL_RGB565,
  };
  return H2_PAL_OK;
}

static h2_pal_result_t h2_web_display_draw(void* user,
                                           const h2_display_rect_t* rect,
                                           const void* pixels,
                                           size_t stride_bytes,
                                           h2_display_pixel_format_t format) {
  H2_WEB_STATE_GUARD();
  h2_web_platform_t* platform = user;
  if (platform != NULL && platform->rgba == NULL) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (format != H2_DISPLAY_PIXEL_RGB565) return H2_DISPLAY_ERR_UNSUPPORTED;
  if (platform == NULL || rect == NULL || pixels == NULL || rect->x < 0 ||
      rect->y < 0 || rect->width <= 0 || rect->height <= 0 ||
      rect->width > platform->width - rect->x ||
      rect->height > platform->height - rect->y ||
      (size_t)rect->width > SIZE_MAX / sizeof(uint16_t) ||
      stride_bytes < (size_t)rect->width * sizeof(uint16_t) ||
      ((size_t)rect->height - 1u) >
          (SIZE_MAX - (size_t)rect->width * 2u) / stride_bytes) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  for (int32_t y = 0; y < rect->height; ++y) {
    const uint8_t* source = (const uint8_t*)pixels + (size_t)y * stride_bytes;
    uint32_t* destination = platform->rgba +
                            (size_t)(rect->y + y) * (size_t)platform->width +
                            (size_t)rect->x;
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

static h2_pal_result_t h2_web_display_present(void* user) {
  H2_WEB_STATE_GUARD();
  h2_web_platform_t* platform = user;
  if (platform == NULL || platform->rgba == NULL) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  return ((int)h2_web_main_call(
              h2_web_present_rgba,
              (const void*[]){&(const uint32_t*){platform->rgba},
                              &(int){platform->width},
                              &(int){platform->height}})
              .i32)
             ? H2_PAL_OK
             : H2_PAL_ERR_UNAVAILABLE;
}

static h2_pal_result_t h2_web_display_brightness(void* user, uint32_t percent) {
  H2_WEB_STATE_GUARD();
  if (user == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (((h2_web_platform_t*)user)->rgba == NULL) return H2_PAL_ERR_INVALID_STATE;
  if (percent > 100u) return H2_PAL_ERR_INVALID_ARG;
  return ((int)h2_web_main_call(h2_web_set_brightness_js,
                                (const void*[]){&(uint32_t){percent}})
              .i32)
             ? H2_PAL_OK
             : H2_PAL_ERR_UNAVAILABLE;
}

static h2_pal_result_t h2_web_display_close(void* user) {
  H2_WEB_STATE_GUARD();
  h2_web_platform_t* platform = user;
  if (platform == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  free(platform->rgba);
  platform->rgba = NULL;
  return H2_PAL_OK;
}

void h2_web_platform_display_init(h2_web_platform_t* platform) {
  H2_WEB_STATE_GUARD();
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

void h2_web_platform_display_deinit(h2_web_platform_t* platform) {
  H2_WEB_STATE_GUARD();
  (void)h2_web_main_call(h2_web_remove_pointer_js,
                         (const void*[]){&(uintptr_t){(uintptr_t)platform}});
  platform->pointer_installed = false;
  (void)h2_web_touch_close(platform);
  (void)h2_web_display_close(platform);
}
