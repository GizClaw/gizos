#include "h2_sdl3_internal.h"

#include <algorithm>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>

namespace {

std::mutex g_provider_mutex;
h2_sdl3_t *g_provider = nullptr;

int init_failure(h2_sdl3_t *state, int result, const char *stage) {
  std::fprintf(stderr,
               "SDL3 display initialization failed: stage=%s result=%d "
               "sdl_error=%s\n",
               stage, result, SDL_GetError());
  state->init_result = result;
  h2_sdl3_cleanup_display(state);
  return result;
}

int init_display(h2_sdl3_t *state) {
  if (state->initialized) {
    return H2_DISPLAY_OK;
  }
  if (state->init_attempted) {
    return state->init_result;
  }
  state->init_attempted = true;
  state->init_result = H2_DISPLAY_ERR_UNAVAILABLE;
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    return init_failure(state, state->init_result, "SDL_Init");
  }
  state->window =
      SDL_CreateWindow(state->title.c_str(), state->width, state->height, 0);
  if (state->window == nullptr) {
    return init_failure(state, state->init_result, "SDL_CreateWindow");
  }
  state->renderer = SDL_CreateRenderer(state->window, nullptr);
  if (state->renderer == nullptr) {
    state->renderer = SDL_CreateRenderer(state->window, "software");
  }
  if (state->renderer == nullptr) {
    return init_failure(state, state->init_result, "SDL_CreateRenderer");
  }
  state->texture = SDL_CreateTexture(state->renderer, SDL_PIXELFORMAT_RGB565,
                                     SDL_TEXTUREACCESS_STREAMING, state->width,
                                     state->height);
  if (state->texture == nullptr) {
    return init_failure(state, state->init_result, "SDL_CreateTexture");
  }
  const size_t width = static_cast<size_t>(state->width);
  const size_t height = static_cast<size_t>(state->height);
  if (height != 0u && width > std::numeric_limits<size_t>::max() / height) {
    return init_failure(state, H2_DISPLAY_ERR_NO_MEMORY, "framebuffer size");
  }
  state->framebuffer =
      static_cast<uint16_t *>(std::calloc(width * height, sizeof(uint16_t)));
  if (state->framebuffer == nullptr) {
    return init_failure(state, H2_DISPLAY_ERR_NO_MEMORY,
                        "framebuffer allocation");
  }
  state->initialized = true;
  state->init_result = H2_DISPLAY_OK;
  return H2_DISPLAY_OK;
}

int display_open(void *user) {
  auto *state = static_cast<h2_sdl3_t *>(user);
  if (state == nullptr || !state->active) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lock(state->display_mutex);
  if (state->close_pending) {
    h2_sdl3_cleanup_display(state);
    state->init_attempted = false;
  }
  const int result = init_display(state);
  if (result == H2_DISPLAY_OK) {
    state->open_count = 1u;
    state->close_pending = false;
  }
  return result;
}

int display_get_info(void *user, h2_display_info_t *info) {
  auto *state = static_cast<h2_sdl3_t *>(user);
  if (state == nullptr || info == nullptr) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lock(state->display_mutex);
  if (!state->initialized || state->open_count == 0u || state->close_pending) {
    return H2_DISPLAY_ERR_INVALID_STATE;
  }
  *info = {state->width, state->height, H2_DISPLAY_PIXEL_RGB565};
  return H2_DISPLAY_OK;
}

int clip_rect(const h2_sdl3_t *state, const h2_display_rect_t *rect,
              h2_display_rect_t *clipped) {
  if (rect->width <= 0 || rect->height <= 0) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  int64_t x1 = rect->x;
  int64_t y1 = rect->y;
  int64_t x2 = x1 + rect->width;
  int64_t y2 = y1 + rect->height;
  x1 = std::max<int64_t>(x1, 0);
  y1 = std::max<int64_t>(y1, 0);
  x2 = std::min<int64_t>(x2, state->width);
  y2 = std::min<int64_t>(y2, state->height);
  if (x1 >= x2 || y1 >= y2) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  *clipped = {static_cast<int>(x1), static_cast<int>(y1),
              static_cast<int>(x2 - x1), static_cast<int>(y2 - y1)};
  return H2_DISPLAY_OK;
}

uint16_t rgb888_to_rgb565(const uint8_t *pixel) {
  return static_cast<uint16_t>(
      ((static_cast<uint16_t>(pixel[0]) & 0xf8u) << 8u) |
      ((static_cast<uint16_t>(pixel[1]) & 0xfcu) << 3u) |
      (static_cast<uint16_t>(pixel[2]) >> 3u));
}

uint16_t rgb444_to_rgb565(uint16_t pixel) {
  const uint16_t red = (pixel >> 8u) & 0x0fu;
  const uint16_t green = (pixel >> 4u) & 0x0fu;
  const uint16_t blue = pixel & 0x0fu;
  return static_cast<uint16_t>((((red << 1u) | (red >> 3u)) << 11u) |
      (((green << 2u) | (green >> 2u)) << 5u) | (blue << 1u) | (blue >> 3u));
}

int display_draw_bitmap(void *user, const h2_display_rect_t *rect,
                        const void *pixels, size_t stride_bytes,
                        h2_display_pixel_format_t format) {
  auto *state = static_cast<h2_sdl3_t *>(user);
  if (state == nullptr || rect == nullptr || pixels == nullptr) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lock(state->display_mutex);
  if (!state->initialized || state->open_count == 0u || state->close_pending) {
    return H2_DISPLAY_ERR_INVALID_STATE;
  }
  // A C caller may supply an unknown integer value. Read its representation
  // before validation; loading an out-of-range C++ enum itself is undefined.
  std::underlying_type_t<h2_display_pixel_format_t> format_code;
  std::memcpy(&format_code, &format, sizeof(format_code));
  size_t pixel_size = 0u;
  switch (format_code) {
  case H2_DISPLAY_PIXEL_RGB565:
  case H2_DISPLAY_PIXEL_RGB444:
    pixel_size = 2u;
    break;
  case H2_DISPLAY_PIXEL_RGB888:
    pixel_size = 3u;
    break;
  default:
    return H2_DISPLAY_ERR_UNSUPPORTED;
  }
  if (rect->width <= 0 || rect->height <= 0 ||
      static_cast<size_t>(rect->width) >
          std::numeric_limits<size_t>::max() / pixel_size) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  const size_t row_bytes = static_cast<size_t>(rect->width) * pixel_size;
  if (stride_bytes < row_bytes ||
      (static_cast<size_t>(rect->height) - 1u) >
          (std::numeric_limits<size_t>::max() - row_bytes) / stride_bytes) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  h2_display_rect_t clipped = {};
  const int clip_result = clip_rect(state, rect, &clipped);
  if (clip_result != H2_DISPLAY_OK) {
    return clip_result;
  }
  const auto *source = static_cast<const uint8_t *>(pixels);
  source += static_cast<size_t>(static_cast<int64_t>(clipped.y) - rect->y) * stride_bytes;
  source += static_cast<size_t>(static_cast<int64_t>(clipped.x) - rect->x) * pixel_size;
  for (int row = 0; row < clipped.height; ++row) {
    uint16_t *destination =
        state->framebuffer +
        static_cast<size_t>(clipped.y + row) *
            static_cast<size_t>(state->width) +
        static_cast<size_t>(clipped.x);
    const uint8_t *source_row =
        source + static_cast<size_t>(row) * stride_bytes;
    if (format_code == H2_DISPLAY_PIXEL_RGB565) {
      std::memcpy(destination, source_row,
                  static_cast<size_t>(clipped.width) * sizeof(uint16_t));
    } else if (format_code == H2_DISPLAY_PIXEL_RGB888) {
      for (int column = 0; column < clipped.width; ++column) {
        destination[column] =
            rgb888_to_rgb565(source_row + static_cast<size_t>(column) * 3u);
      }
    } else {
      for (int column = 0; column < clipped.width; ++column) {
        const size_t offset = static_cast<size_t>(column) * 2u;
        const uint16_t pixel =
            static_cast<uint16_t>(source_row[offset]) |
            static_cast<uint16_t>(source_row[offset + 1u] << 8u);
        destination[column] = rgb444_to_rgb565(pixel);
      }
    }
  }
  return H2_DISPLAY_OK;
}

int display_present(void *user) {
  auto *state = static_cast<h2_sdl3_t *>(user);
  if (state == nullptr) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lock(state->display_mutex);
  if (!state->initialized || state->open_count == 0u || state->close_pending) {
    return H2_DISPLAY_ERR_INVALID_STATE;
  }
  state->present_pending = true;
  return H2_DISPLAY_OK;
}

int display_set_brightness(void *user, uint32_t percent) {
  auto *state = static_cast<h2_sdl3_t *>(user);
  if (state == nullptr) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lock(state->display_mutex);
  if (!state->initialized || state->open_count == 0u || state->close_pending) {
    return H2_DISPLAY_ERR_INVALID_STATE;
  }
  if (percent > 100u) return H2_DISPLAY_ERR_INVALID_ARG;
  state->brightness_mod = static_cast<uint8_t>((percent * 255u + 50u) / 100u);
  state->present_pending = true;
  return H2_DISPLAY_OK;
}

int display_close(void *user) {
  auto *state = static_cast<h2_sdl3_t *>(user);
  if (state == nullptr) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lock(state->display_mutex);
  state->open_count = 0u;
  state->close_pending = true;
  return H2_DISPLAY_OK;
}

const h2_pal_display_vtable_t kDisplayVtable = {
    display_open,    display_get_info,       display_draw_bitmap,
    display_present, display_set_brightness, display_close,
};

} // namespace

void h2_sdl3_cleanup_display(h2_sdl3_t *state) {
  std::free(state->framebuffer);
  state->framebuffer = nullptr;
  SDL_DestroyTexture(state->texture);
  SDL_DestroyRenderer(state->renderer);
  SDL_DestroyWindow(state->window);
  state->texture = nullptr;
  state->renderer = nullptr;
  state->window = nullptr;
  if (SDL_WasInit(SDL_INIT_VIDEO) != 0u) {
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
  }
  state->initialized = false;
  state->present_pending = false;
  state->close_pending = false;
  state->open_count = 0u;
}

int h2_sdl3_present(h2_sdl3_t *state) {
  if (!state->initialized || state->open_count == 0u || state->close_pending) {
    return H2_DISPLAY_ERR_INVALID_STATE;
  }
  if (!SDL_UpdateTexture(state->texture, nullptr, state->framebuffer,
                         state->width * static_cast<int>(sizeof(uint16_t))) ||
      !SDL_SetTextureColorMod(state->texture, state->brightness_mod,
                              state->brightness_mod, state->brightness_mod) ||
      !SDL_RenderClear(state->renderer) ||
      !SDL_RenderTexture(state->renderer, state->texture, nullptr, nullptr)) {
    return H2_DISPLAY_ERR_IO;
  }
  if (state->render_capture != nullptr) {
    SDL_Surface *raw = SDL_RenderReadPixels(state->renderer, nullptr);
    if (raw == nullptr) return H2_DISPLAY_ERR_IO;
    SDL_Surface *rgba = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(raw);
    if (rgba == nullptr) return H2_DISPLAY_ERR_IO;
    const h2_sdl3_render_frame_t frame = {
        static_cast<const uint8_t *>(rgba->pixels),
        static_cast<uint32_t>(rgba->w), static_cast<uint32_t>(rgba->h),
        static_cast<size_t>(rgba->pitch)};
    state->render_capture(state->render_capture_user, &frame);
    SDL_DestroySurface(rgba);
  }
  if (!SDL_RenderPresent(state->renderer)) return H2_DISPLAY_ERR_IO;
  if (state->capture != nullptr) {
    const uint64_t timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    const h2_sdl3_capture_frame_t frame = {
        state->framebuffer, static_cast<uint32_t>(state->width),
        static_cast<uint32_t>(state->height),
        static_cast<size_t>(state->width) * sizeof(uint16_t),
        state->brightness_mod, timestamp};
    state->capture(state->capture_user, &frame);
  }
  return H2_DISPLAY_OK;
}

extern "C" {

h2_pal_result_t h2_sdl3_set_render_capture(h2_sdl3_t *provider,
    h2_sdl3_render_capture_fn callback, void *user) {
  if (provider == nullptr) return H2_PAL_ERR_INVALID_ARG;
  std::lock_guard<std::mutex> lock(provider->display_mutex);
  if (callback != nullptr && provider->render_capture != nullptr)
    return H2_PAL_ERR_BUSY;
  provider->render_capture = callback;
  provider->render_capture_user = user;
  return H2_PAL_OK;
}

h2_pal_result_t h2_sdl3_set_frame_capture(h2_sdl3_t *provider,
                                        h2_sdl3_frame_capture_fn callback,
                                        void *user) {
  if (provider == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lock(provider->display_mutex);
  if (callback != nullptr && provider->capture != nullptr) {
    return H2_PAL_ERR_BUSY;
  }
  provider->capture = callback;
  provider->capture_user = user;
  return H2_PAL_OK;
}


int h2_sdl3_create(const h2_sdl3_config_t *config,
                   h2_sdl3_t **out_provider) {
  if (out_provider == nullptr) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  *out_provider = nullptr;
  if (config == nullptr || config->title == nullptr ||
      config->title[0] == '\0' || config->width <= 0 || config->height <= 0 ||
      config->width > std::numeric_limits<int>::max() /
                          static_cast<int>(sizeof(uint16_t))) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  std::lock_guard<std::mutex> provider_lock(g_provider_mutex);
  if (g_provider != nullptr) {
    return H2_PAL_ERR_BUSY;
  }
  auto *provider = new (std::nothrow) h2_sdl3_t();
  if (provider == nullptr) {
    return H2_DISPLAY_ERR_NO_MEMORY;
  }
  provider->active = true;
  try {
    provider->title = config->title;
  } catch (...) {
    delete provider;
    return H2_DISPLAY_ERR_NO_MEMORY;
  }
  provider->width = config->width;
  provider->height = config->height;
  provider->display = {provider, &kDisplayVtable};
  h2_sdl3_init_touch(provider);
  g_provider = provider;
  *out_provider = provider;
  return H2_DISPLAY_OK;
}

void h2_sdl3_destroy(h2_sdl3_t *provider) {
  if (provider == nullptr) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(provider->display_mutex);
    h2_sdl3_cleanup_display(provider);
  }
  provider->active = false;
  {
    std::lock_guard<std::mutex> provider_lock(g_provider_mutex);
    if (g_provider == provider) {
      g_provider = nullptr;
    }
  }
  delete provider;
}

h2_pal_display_t *h2_sdl3_display(h2_sdl3_t *provider) {
  return provider == nullptr ? nullptr : &provider->display;
}

void h2_sdl3_set_window_title(h2_sdl3_t *provider, const char *title) {
  if (provider == nullptr || title == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(provider->display_mutex);
  try {
    provider->title = title;
  } catch (...) {
    return;
  }
  if (provider->window != nullptr) {
    (void)SDL_SetWindowTitle(provider->window, provider->title.c_str());
  }
}

} // extern "C"
