#include "h2_sdl3_internal.h"

#include <algorithm>
#include <cstring>

namespace {

h2_sdl3_key_t scancode_to_key(SDL_Scancode scancode) {
  switch (scancode) {
  case SDL_SCANCODE_SPACE:
    return H2_SDL3_KEY_SPACE;
  case SDL_SCANCODE_RETURN:
    return H2_SDL3_KEY_ENTER;
  case SDL_SCANCODE_ESCAPE:
    return H2_SDL3_KEY_ESCAPE;
  case SDL_SCANCODE_TAB:
    return H2_SDL3_KEY_TAB;
  case SDL_SCANCODE_BACKSPACE:
    return H2_SDL3_KEY_BACKSPACE;
  case SDL_SCANCODE_DELETE:
    return H2_SDL3_KEY_DELETE;
  case SDL_SCANCODE_PAGEDOWN:
    return H2_SDL3_KEY_PAGE_DOWN;
  case SDL_SCANCODE_PAGEUP:
    return H2_SDL3_KEY_PAGE_UP;
  case SDL_SCANCODE_HOME:
    return H2_SDL3_KEY_HOME;
  case SDL_SCANCODE_END:
    return H2_SDL3_KEY_END;
  case SDL_SCANCODE_KP_PLUS:
    return H2_SDL3_KEY_KP_PLUS;
  case SDL_SCANCODE_KP_MINUS:
    return H2_SDL3_KEY_KP_MINUS;
  case SDL_SCANCODE_UP:
    return H2_SDL3_KEY_UP;
  case SDL_SCANCODE_DOWN:
    return H2_SDL3_KEY_DOWN;
  case SDL_SCANCODE_LEFT:
    return H2_SDL3_KEY_LEFT;
  case SDL_SCANCODE_RIGHT:
    return H2_SDL3_KEY_RIGHT;
  case SDL_SCANCODE_0:
    return H2_SDL3_KEY_DIGIT_0;
  default:
    break;
  }
  if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z) {
    return static_cast<h2_sdl3_key_t>(H2_SDL3_KEY_A + scancode -
                                      SDL_SCANCODE_A);
  }
  if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9) {
    return static_cast<h2_sdl3_key_t>(H2_SDL3_KEY_DIGIT_1 + scancode -
                                      SDL_SCANCODE_1);
  }
  return static_cast<h2_sdl3_key_t>(0);
}

h2_sdl3_event_t make_event(h2_sdl3_event_kind_t kind) {
  h2_sdl3_event_t event = {};
  event.kind = kind;
  return event;
}

h2_pal_result_t touch_open(void *user) {
  auto *provider = static_cast<h2_sdl3_t *>(user);
  if (provider == nullptr || !provider->active) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lock(provider->event_mutex);
  ++provider->touch_open_count;
  return H2_PAL_OK;
}

h2_pal_result_t touch_get_info(void *user, h2_pal_touch_info_t *out_info) {
  auto *provider = static_cast<h2_sdl3_t *>(user);
  if (provider == nullptr || out_info == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lock(provider->event_mutex);
  if (provider->touch_open_count == 0u) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  *out_info = {static_cast<uint32_t>(provider->width),
               static_cast<uint32_t>(provider->height)};
  return H2_PAL_OK;
}

h2_pal_result_t touch_poll(void *user, h2_pal_touch_event_t *out_event) {
  auto *provider = static_cast<h2_sdl3_t *>(user);
  if (provider == nullptr || out_event == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lock(provider->event_mutex);
  if (provider->touch_open_count == 0u) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (provider->touch_count == 0u) {
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  *out_event = provider->touches[provider->touch_head];
  provider->touch_head =
      (provider->touch_head + 1u) % H2_SDL3_TOUCH_QUEUE_CAPACITY;
  --provider->touch_count;
  return H2_PAL_OK;
}

h2_pal_result_t touch_close(void *user) {
  auto *provider = static_cast<h2_sdl3_t *>(user);
  if (provider == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lock(provider->event_mutex);
  if (provider->touch_open_count != 0u) {
    --provider->touch_open_count;
  }
  if (provider->touch_open_count == 0u) {
    provider->touch_head = 0u;
    provider->touch_count = 0u;
  }
  return H2_PAL_OK;
}

const h2_pal_touch_vtable_t kTouchVtable = {
    touch_open,
    touch_get_info,
    touch_poll,
    touch_close,
};

} // namespace

void h2_sdl3_enqueue_event(h2_sdl3_t *provider,
                           const h2_sdl3_event_t &event) {
  std::lock_guard<std::mutex> lock(provider->event_mutex);
  if (provider->event_count == H2_SDL3_EVENT_QUEUE_CAPACITY) {
    provider->event_head =
        (provider->event_head + 1u) % H2_SDL3_EVENT_QUEUE_CAPACITY;
    --provider->event_count;
  }
  const size_t tail = (provider->event_head + provider->event_count) %
                      H2_SDL3_EVENT_QUEUE_CAPACITY;
  provider->events[tail] = event;
  ++provider->event_count;
}

void h2_sdl3_enqueue_touch(h2_sdl3_t *provider,
                           const h2_pal_touch_event_t &event) {
  std::lock_guard<std::mutex> lock(provider->event_mutex);
  if (provider->touch_count == H2_SDL3_TOUCH_QUEUE_CAPACITY) {
    provider->touch_head =
        (provider->touch_head + 1u) % H2_SDL3_TOUCH_QUEUE_CAPACITY;
    --provider->touch_count;
  }
  const size_t tail = (provider->touch_head + provider->touch_count) %
                      H2_SDL3_TOUCH_QUEUE_CAPACITY;
  provider->touches[tail] = event;
  ++provider->touch_count;
}

void h2_sdl3_pump(h2_sdl3_t *provider) {
  if (provider == nullptr || !provider->active) {
    return;
  }
  bool closed = false;
  {
    std::lock_guard<std::mutex> lock(provider->display_mutex);
    if (provider->close_pending) {
      h2_sdl3_cleanup_display(provider);
      provider->init_attempted = false;
      provider->init_result = H2_DISPLAY_ERR_UNAVAILABLE;
      closed = true;
    } else if (provider->present_pending) {
      (void)h2_sdl3_present(provider);
      provider->present_pending = false;
    }
  }
  if (closed) {
    h2_sdl3_enqueue_event(provider, make_event(H2_SDL3_EVENT_CLOSE));
    return;
  }
  if (SDL_WasInit(SDL_INIT_VIDEO) == 0u) {
    return;
  }
  SDL_Event event = {};
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_EVENT_QUIT ||
        event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
      h2_sdl3_enqueue_event(provider, make_event(H2_SDL3_EVENT_CLOSE));
    } else if (event.type == SDL_EVENT_KEY_DOWN ||
               event.type == SDL_EVENT_KEY_UP) {
      const h2_sdl3_key_t key = scancode_to_key(event.key.scancode);
      if (key != 0) {
        h2_sdl3_event_t projected = make_event(H2_SDL3_EVENT_KEY);
        projected.key = key;
        projected.pressed = event.type == SDL_EVENT_KEY_DOWN ? 1 : 0;
        projected.repeat = event.key.repeat ? 1 : 0;
        h2_sdl3_enqueue_event(provider, projected);
      }
    } else if (event.type == SDL_EVENT_TEXT_INPUT) {
      h2_sdl3_event_t projected = make_event(H2_SDL3_EVENT_TEXT);
      std::strncpy(projected.text, event.text.text,
                   sizeof(projected.text) - 1u);
      h2_sdl3_enqueue_event(provider, projected);
    } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
      h2_sdl3_event_t projected = make_event(H2_SDL3_EVENT_WHEEL);
      projected.wheel_x = static_cast<int32_t>(event.wheel.x);
      projected.wheel_y = static_cast<int32_t>(event.wheel.y);
      h2_sdl3_enqueue_event(provider, projected);
    } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
      std::lock_guard<std::mutex> lock(provider->event_mutex);
      provider->pointer.x = static_cast<int32_t>(event.motion.x);
      provider->pointer.y = static_cast<int32_t>(event.motion.y);
      if (provider->pointer.pressed) {
        const h2_pal_touch_event_t touch = {
            H2_PAL_TOUCH_EVENT_MOVE,
            provider->pointer.x,
            provider->pointer.y,
        };
        if (provider->touch_count == H2_SDL3_TOUCH_QUEUE_CAPACITY) {
          provider->touch_head =
              (provider->touch_head + 1u) % H2_SDL3_TOUCH_QUEUE_CAPACITY;
          --provider->touch_count;
        }
        const size_t tail =
            (provider->touch_head + provider->touch_count) %
            H2_SDL3_TOUCH_QUEUE_CAPACITY;
        provider->touches[tail] = touch;
        ++provider->touch_count;
      }
    } else if ((event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                event.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
               event.button.button == SDL_BUTTON_LEFT) {
      const h2_pal_touch_event_kind_t kind =
          event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? H2_PAL_TOUCH_EVENT_DOWN
                                                    : H2_PAL_TOUCH_EVENT_UP;
      {
        std::lock_guard<std::mutex> lock(provider->event_mutex);
        provider->pointer.x = static_cast<int32_t>(event.button.x);
        provider->pointer.y = static_cast<int32_t>(event.button.y);
        provider->pointer.pressed = kind == H2_PAL_TOUCH_EVENT_DOWN ? 1 : 0;
      }
      h2_sdl3_enqueue_touch(
          provider, {kind, static_cast<int32_t>(event.button.x),
                     static_cast<int32_t>(event.button.y)});
    } else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
      bool was_pressed = false;
      int32_t x = 0;
      int32_t y = 0;
      {
        std::lock_guard<std::mutex> lock(provider->event_mutex);
        was_pressed = provider->pointer.pressed != 0;
        x = provider->pointer.x;
        y = provider->pointer.y;
        provider->pointer.pressed = 0;
      }
      if (was_pressed) {
        h2_sdl3_enqueue_touch(provider, {H2_PAL_TOUCH_EVENT_UP, x, y});
      }
      h2_sdl3_enqueue_event(provider, make_event(H2_SDL3_EVENT_FOCUS_LOST));
    } else if (event.type == SDL_EVENT_WINDOW_EXPOSED) {
      std::lock_guard<std::mutex> lock(provider->display_mutex);
      (void)h2_sdl3_present(provider);
    }
  }
}

void h2_sdl3_init_touch(h2_sdl3_t *provider) {
  provider->touch = {provider, &kTouchVtable};
}

extern "C" {

const h2_pal_touch_api_t *h2_sdl3_touch(h2_sdl3_t *provider) {
  if (provider == nullptr) {
    return nullptr;
  }
  return &provider->touch;
}

h2_pal_result_t h2_sdl3_poll_event(h2_sdl3_t *provider,
                                   h2_sdl3_event_t *out_event) {
  if (provider == nullptr || out_event == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_sdl3_pump(provider);
  std::lock_guard<std::mutex> lock(provider->event_mutex);
  if (provider->event_count == 0u) {
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  *out_event = provider->events[provider->event_head];
  provider->event_head =
      (provider->event_head + 1u) % H2_SDL3_EVENT_QUEUE_CAPACITY;
  --provider->event_count;
  return H2_PAL_OK;
}

h2_pal_result_t h2_sdl3_read_pointer(h2_sdl3_t *provider,
                                     h2_sdl3_pointer_state_t *out_state) {
  if (provider == nullptr || out_state == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lock(provider->event_mutex);
  *out_state = provider->pointer;
  return H2_PAL_OK;
}

} // extern "C"
