#include "pointer_touch_adapter.h"

#include <algorithm>

namespace h2::cosmic_drift {

PointerTouchAdapter::PointerTouchAdapter(std::uint32_t width,
                                         std::uint32_t height)
    : width_(width), height_(height), api_{this, nullptr} {
  static const h2_pal_touch_vtable_t vtable = {
      open,
      get_info,
      poll_event,
      close,
  };
  api_.vtable = &vtable;
}

const h2_pal_touch_api_t *PointerTouchAdapter::api() const { return &api_; }

std::int32_t PointerTouchAdapter::clamp_x(std::int32_t x) const {
  return width_ == 0u
             ? 0
             : std::clamp(x, 0, static_cast<std::int32_t>(width_ - 1u));
}

std::int32_t PointerTouchAdapter::clamp_y(std::int32_t y) const {
  return height_ == 0u
             ? 0
             : std::clamp(y, 0, static_cast<std::int32_t>(height_ - 1u));
}

void PointerTouchAdapter::push_locked(h2_pal_touch_event_kind_t kind,
                                      std::int32_t x, std::int32_t y) {
  if (event_count_ > 0u && kind == H2_PAL_TOUCH_EVENT_MOVE) {
    const std::size_t last = (event_head_ + event_count_ - 1u) % events_.size();
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

void PointerTouchAdapter::update(std::int32_t x, std::int32_t y, bool pressed) {
  std::lock_guard<std::mutex> lock(mutex_);
  x = clamp_x(x);
  y = clamp_y(y);
  const bool moved = x != latest_x_ || y != latest_y_;
  latest_x_ = x;
  latest_y_ = y;
  if (!opened_) {
    pressed_ = pressed;
    return;
  }
  if (pressed != pressed_) {
    push_locked(pressed ? H2_PAL_TOUCH_EVENT_DOWN : H2_PAL_TOUCH_EVENT_UP, x,
                y);
  } else if (moved) {
    push_locked(H2_PAL_TOUCH_EVENT_MOVE, x, y);
  }
  pressed_ = pressed;
}

h2_pal_result_t PointerTouchAdapter::open(void *user) {
  auto *adapter = static_cast<PointerTouchAdapter *>(user);
  if (adapter == nullptr || adapter->width_ == 0u || adapter->height_ == 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lock(adapter->mutex_);
  adapter->event_head_ = 0u;
  adapter->event_count_ = 0u;
  adapter->opened_ = true;
  return H2_PAL_OK;
}

h2_pal_result_t PointerTouchAdapter::get_info(void *user,
                                              h2_pal_touch_info_t *out_info) {
  auto *adapter = static_cast<PointerTouchAdapter *>(user);
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

h2_pal_result_t
PointerTouchAdapter::poll_event(void *user, h2_pal_touch_event_t *out_event) {
  auto *adapter = static_cast<PointerTouchAdapter *>(user);
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

h2_pal_result_t PointerTouchAdapter::close(void *user) {
  auto *adapter = static_cast<PointerTouchAdapter *>(user);
  if (adapter == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lock(adapter->mutex_);
  adapter->opened_ = false;
  adapter->pressed_ = false;
  adapter->event_head_ = 0u;
  adapter->event_count_ = 0u;
  return H2_PAL_OK;
}

} // namespace h2::cosmic_drift
