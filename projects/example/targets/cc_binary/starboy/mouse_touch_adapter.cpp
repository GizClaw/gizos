#include "mouse_touch_adapter.h"

#include "global_mouse_source.h"

#include <algorithm>

namespace h2::starboy {

MouseTouchAdapter::MouseTouchAdapter(std::uint32_t width, std::uint32_t height)
    : width_(width), height_(height), api_{this, nullptr} {
  static const h2_pal_touch_vtable_t vtable = {
      open,
      get_info,
      poll_event,
      close,
  };
  api_.vtable = &vtable;
}

const h2_pal_touch_api_t *MouseTouchAdapter::api() const { return &api_; }

std::int32_t MouseTouchAdapter::clamp_x(std::int32_t x) const {
  if (width_ == 0u) {
    return 0;
  }
  return std::clamp(x, 0, static_cast<std::int32_t>(width_ - 1u));
}

std::int32_t MouseTouchAdapter::clamp_y(std::int32_t y) const {
  if (height_ == 0u) {
    return 0;
  }
  return std::clamp(y, 0, static_cast<std::int32_t>(height_ - 1u));
}

void MouseTouchAdapter::push_locked(h2_pal_touch_event_kind_t kind,
                                    std::int32_t x, std::int32_t y) {
  if (event_count_ > 0u && kind == H2_PAL_TOUCH_EVENT_MOVE) {
    const std::size_t last =
        (event_head_ + event_count_ - 1u) % events_.size();
    if (events_[last].kind == H2_PAL_TOUCH_EVENT_MOVE) {
      events_[last] = {kind, x, y};
      return;
    }
  }
  if (event_count_ == events_.size()) {
    event_head_ = (event_head_ + 1u) % events_.size();
    --event_count_;
  }
  const std::size_t tail = (event_head_ + event_count_) % events_.size();
  events_[tail] = {kind, x, y};
  ++event_count_;
}

void MouseTouchAdapter::update(bool available, std::int32_t x,
                               std::int32_t y) {
  std::lock_guard<std::mutex> lock(mutex_);
  x = clamp_x(x);
  y = clamp_y(y);
  const bool moved = x != latest_x_ || y != latest_y_;
  latest_x_ = x;
  latest_y_ = y;
  available_ = available;
  if (!opened_) {
    return;
  }
  if (!available) {
    if (contact_active_) {
      push_locked(H2_PAL_TOUCH_EVENT_UP, latest_x_, latest_y_);
      contact_active_ = false;
    }
    return;
  }
  if (!contact_active_) {
    push_locked(H2_PAL_TOUCH_EVENT_DOWN, latest_x_, latest_y_);
    contact_active_ = true;
  } else if (moved) {
    push_locked(H2_PAL_TOUCH_EVENT_MOVE, latest_x_, latest_y_);
  }
}

void MouseTouchAdapter::update_global_coordinates(
    std::int32_t global_x, std::int32_t global_y,
    std::int32_t window_x, std::int32_t window_y) {
  update(true, global_x - window_x, global_y - window_y);
}

bool MouseTouchAdapter::bind_global_window(const char *window_title) {
  native_window_ = find_global_mouse_window(window_title);
  return native_window_ != nullptr;
}

bool MouseTouchAdapter::sample_global_mouse() {
  if (native_window_ == nullptr) {
    return false;
  }
  std::int32_t relative_x = 0;
  std::int32_t relative_y = 0;
  if (!sample_global_mouse_relative(
          native_window_, &relative_x, &relative_y)) {
    return false;
  }
  update(true, relative_x, relative_y);
  return true;
}

h2_pal_result_t MouseTouchAdapter::open(void *user) {
  auto *adapter = static_cast<MouseTouchAdapter *>(user);
  if (adapter == nullptr || adapter->width_ == 0u || adapter->height_ == 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lock(adapter->mutex_);
  adapter->event_head_ = 0u;
  adapter->event_count_ = 0u;
  adapter->opened_ = true;
  adapter->contact_active_ = false;
  if (adapter->available_) {
    adapter->push_locked(H2_PAL_TOUCH_EVENT_DOWN,
                         adapter->latest_x_, adapter->latest_y_);
    adapter->contact_active_ = true;
  }
  return H2_PAL_OK;
}

h2_pal_result_t MouseTouchAdapter::get_info(
    void *user, h2_pal_touch_info_t *out_info) {
  auto *adapter = static_cast<MouseTouchAdapter *>(user);
  if (adapter == nullptr || out_info == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lock(adapter->mutex_);
  if (!adapter->opened_) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  *out_info = {adapter->width_, adapter->height_};
  return H2_PAL_OK;
}

h2_pal_result_t MouseTouchAdapter::poll_event(
    void *user, h2_pal_touch_event_t *out_event) {
  auto *adapter = static_cast<MouseTouchAdapter *>(user);
  if (adapter == nullptr || out_event == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lock(adapter->mutex_);
  if (!adapter->opened_) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (adapter->event_count_ == 0u) {
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  *out_event = adapter->events_[adapter->event_head_];
  adapter->event_head_ = (adapter->event_head_ + 1u) % adapter->events_.size();
  --adapter->event_count_;
  return H2_PAL_OK;
}

h2_pal_result_t MouseTouchAdapter::close(void *user) {
  auto *adapter = static_cast<MouseTouchAdapter *>(user);
  if (adapter == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lock(adapter->mutex_);
  adapter->opened_ = false;
  adapter->contact_active_ = false;
  adapter->event_head_ = 0u;
  adapter->event_count_ = 0u;
  return H2_PAL_OK;
}

}  // namespace h2::starboy
