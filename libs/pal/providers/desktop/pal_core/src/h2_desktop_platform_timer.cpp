#include "h2_desktop_platform.h"
#include "h2_desktop_resource_stats_internal.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <new>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;

struct DesktopTimer {
  std::mutex mutex;
  std::condition_variable changed;
  std::thread worker;
  h2_pal_timer_cb_t callback = nullptr;
  void *callback_user = nullptr;
  uint32_t period_ms = 0u;
  uint64_t generation = 0u;
  Clock::time_point deadline;
  bool repeat = false;
  bool running = false;
  bool in_callback = false;
  bool stopping = false;
  bool destroying = false;
  bool joining = false;
  bool self_cleanup = false;
};

void dispose(DesktopTimer *timer) {
  delete timer;
  h2_desktop_resource_release(h2_desktop_resource_kind::timer);
}

void arm(DesktopTimer *timer) {
  timer->running = true;
  timer->deadline = Clock::now() + std::chrono::milliseconds(timer->period_ms);
  ++timer->generation;
  timer->changed.notify_all();
}

void timer_worker(DesktopTimer *timer) {
  std::unique_lock<std::mutex> lock(timer->mutex);
  while (!timer->destroying) {
    timer->changed.wait(
        lock, [timer] { return timer->running || timer->destroying; });
    if (timer->destroying)
      break;
    const uint64_t generation = timer->generation;
    const Clock::time_point deadline = timer->deadline;
    if (timer->changed.wait_until(lock, deadline, [timer, generation] {
          return !timer->running || timer->destroying ||
                 timer->generation != generation;
        }))
      continue;
    if (!timer->repeat)
      timer->running = false;
    timer->in_callback = true;
    lock.unlock();
    timer->callback(timer->callback_user,
                    reinterpret_cast<h2_pal_timer_t *>(timer));
    lock.lock();
    timer->in_callback = false;
    timer->changed.notify_all();
    if (timer->repeat && timer->running && !timer->destroying &&
        timer->generation == generation) {
      // Keep the phase, skipping elapsed periods instead of replaying a burst
      // after a slow callback. A callback reset/start owns its new deadline.
      const auto period = std::chrono::milliseconds(timer->period_ms);
      const auto now = Clock::now();
      const auto skipped = (now - deadline) / period + 1;
      timer->deadline = deadline + period * skipped;
    }
  }
  const bool self_cleanup = timer->self_cleanup;
  lock.unlock();
  if (self_cleanup)
    dispose(timer);
}

h2_pal_result_t timer_create(void *, const h2_pal_timer_config_t *config,
                             h2_pal_timer_t **out_timer) {
  if (out_timer != nullptr)
    *out_timer = nullptr;
  if (config == nullptr || out_timer == nullptr || config->cb == nullptr ||
      config->period_ms == 0u ||
      (config->flags &
       ~(H2_PAL_TIMER_FLAG_REPEAT | H2_PAL_TIMER_FLAG_AUTO_START)) != 0u)
    return H2_PAL_ERR_INVALID_ARG;
  DesktopTimer *timer = new (std::nothrow) DesktopTimer();
  if (timer == nullptr)
    return H2_PAL_ERR_NO_MEMORY;
  timer->callback = config->cb;
  timer->callback_user = config->cb_user;
  timer->period_ms = config->period_ms;
  timer->repeat = (config->flags & H2_PAL_TIMER_FLAG_REPEAT) != 0u;
  // A started worker cannot enter a callback before its std::thread handle and
  // resource ownership have both been published under this lock.
  std::unique_lock<std::mutex> lock(timer->mutex);
  if ((config->flags & H2_PAL_TIMER_FLAG_AUTO_START) != 0u)
    arm(timer);
  try {
    timer->worker = std::thread(timer_worker, timer);
  } catch (...) {
    lock.unlock();
    delete timer;
    return H2_PAL_ERR_UNAVAILABLE;
  }
  h2_desktop_resource_acquire(h2_desktop_resource_kind::timer);
  *out_timer = reinterpret_cast<h2_pal_timer_t *>(timer);
  return H2_PAL_OK;
}

h2_pal_result_t timer_destroy(void *, h2_pal_timer_t *raw_timer) {
  if (raw_timer == nullptr)
    return H2_PAL_ERR_INVALID_ARG;
  auto *timer = reinterpret_cast<DesktopTimer *>(raw_timer);
  std::unique_lock<std::mutex> lock(timer->mutex);
  if (timer->stopping || timer->joining || timer->self_cleanup)
    return H2_PAL_ERR_BUSY;
  if (timer->worker.get_id() == std::this_thread::get_id()) {
    try {
      timer->worker.detach();
    } catch (...) {
      return H2_PAL_ERR_IO;
    }
    timer->self_cleanup = true;
    timer->destroying = true;
    timer->running = false;
    ++timer->generation;
    return H2_PAL_OK;
  }
  timer->destroying = true;
  timer->running = false;
  timer->joining = true;
  ++timer->generation;
  timer->changed.notify_all();
  lock.unlock();
  try {
    timer->worker.join();
  } catch (...) {
    lock.lock();
    timer->joining = false;
    return H2_PAL_ERR_IO;
  }
  dispose(timer);
  return H2_PAL_OK;
}

h2_pal_result_t timer_start(void *, h2_pal_timer_t *raw_timer) {
  if (raw_timer == nullptr)
    return H2_PAL_ERR_INVALID_ARG;
  auto *timer = reinterpret_cast<DesktopTimer *>(raw_timer);
  std::lock_guard<std::mutex> lock(timer->mutex);
  if (timer->destroying)
    return H2_PAL_ERR_INVALID_STATE;
  if (timer->stopping)
    return H2_PAL_ERR_BUSY;
  if (timer->running)
    return H2_PAL_ERR_INVALID_STATE;
  arm(timer);
  return H2_PAL_OK;
}

h2_pal_result_t timer_stop(void *, h2_pal_timer_t *raw_timer) {
  if (raw_timer == nullptr)
    return H2_PAL_ERR_INVALID_ARG;
  auto *timer = reinterpret_cast<DesktopTimer *>(raw_timer);
  std::unique_lock<std::mutex> lock(timer->mutex);
  if (timer->destroying)
    return H2_PAL_ERR_INVALID_STATE;
  const bool self = timer->worker.get_id() == std::this_thread::get_id();
  if (timer->stopping && !self)
    return H2_PAL_ERR_BUSY;
  timer->running = false;
  ++timer->generation;
  timer->changed.notify_all();
  if (!self) {
    timer->stopping = true;
    timer->changed.wait(lock, [timer] { return !timer->in_callback; });
    timer->stopping = false;
  }
  return H2_PAL_OK;
}

h2_pal_result_t timer_reset(void *, h2_pal_timer_t *raw_timer) {
  if (raw_timer == nullptr)
    return H2_PAL_ERR_INVALID_ARG;
  auto *timer = reinterpret_cast<DesktopTimer *>(raw_timer);
  std::lock_guard<std::mutex> lock(timer->mutex);
  if (timer->destroying)
    return H2_PAL_ERR_INVALID_STATE;
  if (timer->stopping)
    return H2_PAL_ERR_BUSY;
  arm(timer);
  return H2_PAL_OK;
}

h2_pal_result_t timer_set_period(void *, h2_pal_timer_t *raw_timer,
                                 uint32_t period_ms) {
  if (raw_timer == nullptr || period_ms == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  auto *timer = reinterpret_cast<DesktopTimer *>(raw_timer);
  std::lock_guard<std::mutex> lock(timer->mutex);
  if (timer->destroying)
    return H2_PAL_ERR_INVALID_STATE;
  if (timer->stopping)
    return H2_PAL_ERR_BUSY;
  timer->period_ms = period_ms;
  if (timer->running)
    arm(timer);
  return H2_PAL_OK;
}

h2_pal_result_t timer_is_running(void *, h2_pal_timer_t *raw_timer,
                                 int *out_running) {
  if (out_running != nullptr)
    *out_running = 0;
  if (raw_timer == nullptr || out_running == nullptr)
    return H2_PAL_ERR_INVALID_ARG;
  auto *timer = reinterpret_cast<DesktopTimer *>(raw_timer);
  std::lock_guard<std::mutex> lock(timer->mutex);
  if (timer->destroying)
    return H2_PAL_ERR_INVALID_STATE;
  *out_running = timer->running ? 1 : 0;
  return H2_PAL_OK;
}

const h2_pal_timer_vtable_t timer_vtable = {
    timer_create, timer_destroy,    timer_start,     timer_stop,
    timer_reset,  timer_set_period, timer_is_running};
const h2_pal_timer_api_t timer_api = {nullptr, &timer_vtable};
} // namespace

extern "C" const h2_pal_timer_api_t *h2_desktop_platform_timer_api(void) {
  return &timer_api;
}
