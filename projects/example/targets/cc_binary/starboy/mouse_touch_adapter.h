#ifndef H2_STARBOY_MOUSE_TOUCH_ADAPTER_H
#define H2_STARBOY_MOUSE_TOUCH_ADAPTER_H

#include "h2/pal/hal/h2_pal_touch.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace h2::starboy {

class MouseTouchAdapter {
 public:
  MouseTouchAdapter(std::uint32_t width, std::uint32_t height);
  MouseTouchAdapter(const MouseTouchAdapter &) = delete;
  MouseTouchAdapter &operator=(const MouseTouchAdapter &) = delete;

  const h2_pal_touch_api_t *api() const;

  // Mouse hover is intentionally independent of the native button state.
  void update(bool available, std::int32_t x, std::int32_t y);
  void update_global_coordinates(std::int32_t global_x,
                                 std::int32_t global_y,
                                 std::int32_t window_x,
                                 std::int32_t window_y);

  // These functions must run on the SDL main thread.
  bool bind_global_window(const char *window_title);
  bool sample_global_mouse();

 private:
  static constexpr std::size_t kEventCapacity = 8u;

  static h2_pal_result_t open(void *user);
  static h2_pal_result_t get_info(
      void *user, h2_pal_touch_info_t *out_info);
  static h2_pal_result_t poll_event(
      void *user, h2_pal_touch_event_t *out_event);
  static h2_pal_result_t close(void *user);

  void push_locked(h2_pal_touch_event_kind_t kind,
                   std::int32_t x, std::int32_t y);
  std::int32_t clamp_x(std::int32_t x) const;
  std::int32_t clamp_y(std::int32_t y) const;

  std::uint32_t width_;
  std::uint32_t height_;
  mutable std::mutex mutex_;
  h2_pal_touch_api_t api_;
  std::array<h2_pal_touch_event_t, kEventCapacity> events_{};
  std::size_t event_head_ = 0u;
  std::size_t event_count_ = 0u;
  std::int32_t latest_x_ = 0;
  std::int32_t latest_y_ = 0;
  bool opened_ = false;
  bool available_ = false;
  bool contact_active_ = false;
  void *native_window_ = nullptr;
};

}  // namespace h2::starboy

#endif
