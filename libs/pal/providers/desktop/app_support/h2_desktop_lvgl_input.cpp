#include "h2_desktop_lvgl_input.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>

struct H2DesktopLvglInput {
  lv_indev_t *keyboard = nullptr;
  lv_indev_t *wheel = nullptr;
  std::array<std::uint32_t, 64> keys = {};
  std::size_t key_head = 0u;
  std::size_t key_count = 0u;
  std::uint32_t last_key = 0u;
  bool key_release_pending = false;
  std::int16_t wheel_diff = 0;
};

namespace {

void keyboard_read(lv_indev_t *indev, lv_indev_data_t *data) {
  auto *input = static_cast<H2DesktopLvglInput *>(
      lv_indev_get_driver_data(indev));
  if (input->key_release_pending) {
    data->key = input->last_key;
    data->state = LV_INDEV_STATE_RELEASED;
    input->key_release_pending = false;
  } else if (input->key_count == 0u) {
    data->key = input->last_key;
    data->state = LV_INDEV_STATE_RELEASED;
  } else {
    input->last_key = input->keys[input->key_head];
    input->key_head = (input->key_head + 1u) % input->keys.size();
    --input->key_count;
    input->key_release_pending = true;
    data->key = input->last_key;
    data->state = LV_INDEV_STATE_PRESSED;
  }
}

void wheel_read(lv_indev_t *indev, lv_indev_data_t *data) {
  auto *input = static_cast<H2DesktopLvglInput *>(
      lv_indev_get_driver_data(indev));
  data->enc_diff = input->wheel_diff;
  data->state = LV_INDEV_STATE_RELEASED;
  input->wheel_diff = 0;
}

std::uint32_t control_key(h2_sdl3_key_t key) {
  switch (key) {
  case H2_SDL3_KEY_RIGHT:
    return LV_KEY_RIGHT;
  case H2_SDL3_KEY_LEFT:
    return LV_KEY_LEFT;
  case H2_SDL3_KEY_UP:
    return LV_KEY_UP;
  case H2_SDL3_KEY_DOWN:
    return LV_KEY_DOWN;
  case H2_SDL3_KEY_ESCAPE:
    return LV_KEY_ESC;
  case H2_SDL3_KEY_BACKSPACE:
    return LV_KEY_BACKSPACE;
  case H2_SDL3_KEY_DELETE:
    return LV_KEY_DEL;
  case H2_SDL3_KEY_ENTER:
    return LV_KEY_ENTER;
  case H2_SDL3_KEY_TAB:
  case H2_SDL3_KEY_PAGE_DOWN:
    return LV_KEY_NEXT;
  case H2_SDL3_KEY_PAGE_UP:
    return LV_KEY_PREV;
  case H2_SDL3_KEY_HOME:
    return LV_KEY_HOME;
  case H2_SDL3_KEY_END:
    return LV_KEY_END;
  case H2_SDL3_KEY_KP_PLUS:
    return LV_KEY_RIGHT;
  case H2_SDL3_KEY_KP_MINUS:
    return LV_KEY_LEFT;
  default:
    return 0u;
  }
}

bool queue_key(H2DesktopLvglInput *input, std::uint32_t key) {
  if (key == 0u || input->key_count == input->keys.size()) {
    return false;
  }
  const std::size_t tail =
      (input->key_head + input->key_count) % input->keys.size();
  input->keys[tail] = key;
  ++input->key_count;
  return true;
}

bool is_utf8_continuation(std::uint8_t byte) {
  return (byte & 0xc0u) == 0x80u;
}

std::size_t decode_utf8(const std::uint8_t *text, std::size_t remaining,
                        std::uint32_t *out_code_point) {
  const std::uint8_t first = text[0];
  *out_code_point = 0u;
  if (first <= 0x7fu) {
    *out_code_point = first;
    return 1u;
  }
  if (first >= 0xc2u && first <= 0xdfu && remaining >= 2u &&
      is_utf8_continuation(text[1])) {
    *out_code_point = ((first & 0x1fu) << 6u) | (text[1] & 0x3fu);
    return 2u;
  }
  if (first >= 0xe0u && first <= 0xefu && remaining >= 3u &&
      is_utf8_continuation(text[1]) && is_utf8_continuation(text[2]) &&
      (first != 0xe0u || text[1] >= 0xa0u) &&
      (first != 0xedu || text[1] <= 0x9fu)) {
    *out_code_point = ((first & 0x0fu) << 12u) |
                      ((text[1] & 0x3fu) << 6u) | (text[2] & 0x3fu);
    return 3u;
  }
  if (first >= 0xf0u && first <= 0xf4u && remaining >= 4u &&
      is_utf8_continuation(text[1]) && is_utf8_continuation(text[2]) &&
      is_utf8_continuation(text[3]) &&
      (first != 0xf0u || text[1] >= 0x90u) &&
      (first != 0xf4u || text[1] <= 0x8fu)) {
    *out_code_point = ((first & 0x07u) << 18u) |
                      ((text[1] & 0x3fu) << 12u) |
                      ((text[2] & 0x3fu) << 6u) | (text[3] & 0x3fu);
    return 4u;
  }
  return 1u;
}

void drain_keyboard(H2DesktopLvglInput *input, std::size_t count) {
  for (std::size_t index = 0u; index < count; ++index) {
    lv_indev_read(input->keyboard);
    lv_indev_read(input->keyboard);
  }
}

void queue_text(H2DesktopLvglInput *input, const char *text) {
  if (text == nullptr) {
    return;
  }
  const auto *cursor = reinterpret_cast<const std::uint8_t *>(text);
  std::size_t remaining = std::strlen(text);
  while (remaining != 0u) {
    std::uint32_t code_point = 0u;
    const std::size_t count = decode_utf8(cursor, remaining, &code_point);
    if (code_point != 0u) {
      if (!queue_key(input, code_point)) {
        drain_keyboard(input, input->key_count);
        if (!queue_key(input, code_point)) {
          return;
        }
      }
    }
    cursor += count;
    remaining -= count;
  }
  drain_keyboard(input, input->key_count);
}

} // namespace

int h2_desktop_lvgl_input_create(lv_display_t *display,
                                 H2DesktopLvglInput **out_input) {
  if (display == nullptr || out_input == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_input = nullptr;
  auto *input = new (std::nothrow) H2DesktopLvglInput();
  if (input == nullptr) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  input->keyboard = lv_indev_create();
  input->wheel = lv_indev_create();
  if (input->keyboard == nullptr || input->wheel == nullptr) {
    h2_desktop_lvgl_input_destroy(input);
    return H2_PAL_ERR_NO_MEMORY;
  }
  lv_indev_set_type(input->keyboard, LV_INDEV_TYPE_KEYPAD);
  lv_indev_set_read_cb(input->keyboard, keyboard_read);
  lv_indev_set_driver_data(input->keyboard, input);
  lv_indev_set_display(input->keyboard, display);
  lv_indev_set_mode(input->keyboard, LV_INDEV_MODE_EVENT);
  lv_indev_set_type(input->wheel, LV_INDEV_TYPE_ENCODER);
  lv_indev_set_read_cb(input->wheel, wheel_read);
  lv_indev_set_driver_data(input->wheel, input);
  lv_indev_set_display(input->wheel, display);
  lv_indev_set_mode(input->wheel, LV_INDEV_MODE_EVENT);
  *out_input = input;
  return H2_PAL_OK;
}

void h2_desktop_lvgl_input_destroy(H2DesktopLvglInput *input) {
  if (input == nullptr) {
    return;
  }
  if (input->keyboard != nullptr) {
    lv_indev_delete(input->keyboard);
  }
  if (input->wheel != nullptr) {
    lv_indev_delete(input->wheel);
  }
  delete input;
}

void h2_desktop_lvgl_input_handle(H2DesktopLvglInput *input,
                                  const h2_sdl3_event_t &event) {
  if (input == nullptr) {
    return;
  }
  if (event.kind == H2_SDL3_EVENT_FOCUS_LOST) {
    input->key_count = 0u;
    input->key_head = 0u;
    if (input->key_release_pending) {
      lv_indev_read(input->keyboard);
    }
    input->key_release_pending = false;
    input->wheel_diff = 0;
    lv_indev_read(input->wheel);
  } else if (event.kind == H2_SDL3_EVENT_KEY && event.pressed != 0 &&
             event.repeat == 0 && queue_key(input, control_key(event.key))) {
    lv_indev_read(input->keyboard);
    lv_indev_read(input->keyboard);
  } else if (event.kind == H2_SDL3_EVENT_TEXT) {
    queue_text(input, event.text);
  } else if (event.kind == H2_SDL3_EVENT_WHEEL) {
    const std::int64_t total =
        static_cast<std::int64_t>(input->wheel_diff) - event.wheel_y;
    input->wheel_diff = static_cast<std::int16_t>(std::clamp<std::int64_t>(
        total, std::numeric_limits<std::int16_t>::min(),
        std::numeric_limits<std::int16_t>::max()));
    lv_indev_read(input->wheel);
  }
}

lv_indev_t *h2_desktop_lvgl_input_keyboard(H2DesktopLvglInput *input) {
  return input == nullptr ? nullptr : input->keyboard;
}

lv_indev_t *h2_desktop_lvgl_input_wheel(H2DesktopLvglInput *input) {
  return input == nullptr ? nullptr : input->wheel;
}
