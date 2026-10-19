#include "h2_desktop_platform.h"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
const h2_pal_timer_api_t *api;

struct Calls {
  std::mutex mutex;
  std::condition_variable changed;
  size_t count = 0u;
  bool self_stop = false;
  bool self_destroy = false;
  h2_pal_result_t result = H2_PAL_ERR_INVALID_STATE;
};
void callback(void *user, h2_pal_timer_t *timer) {
  auto *calls = static_cast<Calls *>(user);
  h2_pal_result_t result = H2_PAL_OK;
  if (calls->self_stop)
    result = h2_pal_timer_stop(api, timer);
  if (calls->self_destroy)
    result = h2_pal_timer_destroy(api, timer);
  std::lock_guard<std::mutex> lock(calls->mutex);
  calls->result = result;
  ++calls->count;
  calls->changed.notify_all();
}
void wait_for(Calls &calls, size_t count) {
  std::unique_lock<std::mutex> lock(calls.mutex);
  assert(
      calls.changed.wait_for(lock, 2s, [&] { return calls.count >= count; }));
}
size_t count(Calls &calls) {
  std::lock_guard<std::mutex> lock(calls.mutex);
  return calls.count;
}
void quiet(Calls &calls, size_t expected) {
  std::unique_lock<std::mutex> lock(calls.mutex);
  assert(!calls.changed.wait_for(lock, 30ms,
                                 [&] { return calls.count != expected; }));
}
size_t live_timers() {
  h2_desktop_platform_resource_stats_t stats{};
  assert(h2_desktop_platform_get_resource_stats(&stats) == H2_PAL_OK);
  return stats.live_timers;
}
void wait_for_reclamation(size_t baseline) {
  const auto deadline = Clock::now() + 2s;
  while (live_timers() != baseline && Clock::now() < deadline)
    std::this_thread::yield();
  assert(live_timers() == baseline);
}

void test_validation() {
  Calls calls;
  h2_pal_timer_config_t config = {"timer", 10u, 0u, callback, &calls};
  h2_pal_timer_t *timer = reinterpret_cast<h2_pal_timer_t *>(1);
  assert(api->vtable->create(api->user, nullptr, &timer) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(timer == nullptr);
  config.period_ms = 0u;
  assert(api->vtable->create(api->user, &config, &timer) ==
         H2_PAL_ERR_INVALID_ARG);
  config.period_ms = 10u;
  config.flags = 0x80000000u;
  assert(api->vtable->create(api->user, &config, &timer) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(timer == nullptr);
  int running = 99;
  assert(api->vtable->is_running(api->user, nullptr, &running) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(running == 0);
  assert(api->vtable->start(api->user, nullptr) == H2_PAL_ERR_INVALID_ARG);
  assert(api->vtable->stop(api->user, nullptr) == H2_PAL_ERR_INVALID_ARG);
  assert(api->vtable->reset(api->user, nullptr) == H2_PAL_ERR_INVALID_ARG);
  assert(api->vtable->set_period_ms(api->user, nullptr, 1u) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(api->vtable->destroy(api->user, nullptr) == H2_PAL_ERR_INVALID_ARG);
}

void test_one_shot() {
  const size_t baseline = live_timers();
  Calls calls;
  const h2_pal_timer_config_t config = {"one-shot", 1000u, 0u, callback,
                                        &calls};
  h2_pal_timer_t *timer = nullptr;
  assert(h2_pal_timer_create(api, &config, &timer) == H2_PAL_OK);
  assert(live_timers() == baseline + 1u);
  int running = -1;
  assert(h2_pal_timer_is_running(api, timer, &running) == H2_PAL_OK &&
         !running);
  assert(h2_pal_timer_set_period_ms(api, timer, 10u) == H2_PAL_OK);
  quiet(calls, 0u);
  assert(h2_pal_timer_start(api, timer) == H2_PAL_OK);
  assert(h2_pal_timer_start(api, timer) == H2_PAL_ERR_INVALID_STATE);
  wait_for(calls, 1u);
  assert(h2_pal_timer_is_running(api, timer, &running) == H2_PAL_OK &&
         !running);
  quiet(calls, 1u);
  assert(h2_pal_timer_reset(api, timer) == H2_PAL_OK);
  wait_for(calls, 2u);
  assert(h2_pal_timer_set_period_ms(api, timer, 1000u) == H2_PAL_OK);
  assert(h2_pal_timer_start(api, timer) == H2_PAL_OK);
  assert(h2_pal_timer_set_period_ms(api, timer, 5u) == H2_PAL_OK);
  wait_for(calls,
           3u); // A running period change rearms, rather than waiting 1s.
  assert(h2_pal_timer_set_period_ms(api, timer, 0u) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_pal_timer_stop(api, timer) == H2_PAL_OK);
  assert(h2_pal_timer_stop(api, timer) == H2_PAL_OK);
  assert(h2_pal_timer_destroy(api, timer) == H2_PAL_OK);
  quiet(calls, 3u);
  assert(live_timers() == baseline);
}

void test_repeat_and_generation() {
  const size_t baseline = live_timers();
  Calls calls;
  const h2_pal_timer_config_t config = {
      "repeat", 5u, H2_PAL_TIMER_FLAG_AUTO_START | H2_PAL_TIMER_FLAG_REPEAT,
      callback, &calls};
  h2_pal_timer_t *timer = nullptr;
  assert(h2_pal_timer_create(api, &config, &timer) == H2_PAL_OK);
  wait_for(calls, 3u);
  assert(h2_pal_timer_stop(api, timer) == H2_PAL_OK);
  const size_t stopped = count(calls);
  quiet(calls, stopped);
  assert(h2_pal_timer_set_period_ms(api, timer, 1000u) == H2_PAL_OK);
  for (int i = 0; i < 100; ++i) {
    assert(h2_pal_timer_start(api, timer) == H2_PAL_OK);
    assert(h2_pal_timer_reset(api, timer) == H2_PAL_OK);
    assert(h2_pal_timer_stop(api, timer) == H2_PAL_OK);
  }
  quiet(calls, stopped);
  assert(h2_pal_timer_set_period_ms(api, timer, 5u) == H2_PAL_OK);
  assert(h2_pal_timer_reset(api, timer) == H2_PAL_OK);
  wait_for(calls, stopped + 2u);
  assert(h2_pal_timer_destroy(api, timer) == H2_PAL_OK);
  quiet(calls, count(calls));
  assert(live_timers() == baseline);
}

struct Gate {
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false;
  bool check_busy = false;
  bool checked = false;
  bool release = false;
  bool finished = false;
  h2_pal_result_t rearm_result = H2_PAL_OK;
  h2_pal_result_t destroy_result = H2_PAL_OK;
};
void gated_callback(void *user, h2_pal_timer_t *timer) {
  auto *gate = static_cast<Gate *>(user);
  std::unique_lock<std::mutex> lock(gate->mutex);
  gate->entered = true;
  gate->changed.notify_all();
  assert(gate->changed.wait_for(
      lock, 2s, [&] { return gate->check_busy || gate->release; }));
  if (gate->check_busy) {
    lock.unlock();
    const auto rearm = h2_pal_timer_reset(api, timer);
    const auto destroy = h2_pal_timer_destroy(api, timer);
    lock.lock();
    gate->rearm_result = rearm;
    gate->destroy_result = destroy;
    gate->checked = true;
    gate->changed.notify_all();
    assert(gate->changed.wait_for(lock, 2s, [&] { return gate->release; }));
  }
}

void test_external_stop_waits() {
  const size_t baseline = live_timers();
  Gate gate;
  const h2_pal_timer_config_t config = {
      "in-flight", 1u, H2_PAL_TIMER_FLAG_REPEAT | H2_PAL_TIMER_FLAG_AUTO_START,
      gated_callback, &gate};
  h2_pal_timer_t *timer = nullptr;
  assert(h2_pal_timer_create(api, &config, &timer) == H2_PAL_OK);
  {
    std::unique_lock<std::mutex> lock(gate.mutex);
    assert(gate.changed.wait_for(lock, 2s, [&] { return gate.entered; }));
  }
  std::thread stopper([&] {
    assert(h2_pal_timer_stop(api, timer) == H2_PAL_OK);
    std::lock_guard<std::mutex> lock(gate.mutex);
    gate.finished = true;
    gate.changed.notify_all();
  });
  const auto deadline = Clock::now() + 2s;
  int running = 1;
  while (running && Clock::now() < deadline) {
    assert(h2_pal_timer_is_running(api, timer, &running) == H2_PAL_OK);
    std::this_thread::yield();
  }
  assert(!running);
  {
    std::unique_lock<std::mutex> lock(gate.mutex);
    assert(
        !gate.finished); // stop has cancelled admission but cannot return yet.
    gate.check_busy = true;
    gate.changed.notify_all();
    assert(gate.changed.wait_for(lock, 2s, [&] { return gate.checked; }));
    assert(gate.rearm_result == H2_PAL_ERR_BUSY);
    assert(gate.destroy_result == H2_PAL_ERR_BUSY);
    gate.release = true;
    gate.changed.notify_all();
  }
  stopper.join();
  assert(h2_pal_timer_destroy(api, timer) == H2_PAL_OK);
  assert(live_timers() == baseline);
}

void test_external_destroy_waits() {
  const size_t baseline = live_timers();
  Gate gate;
  const h2_pal_timer_config_t config = {"destroy-flight", 1u,
                                        H2_PAL_TIMER_FLAG_AUTO_START,
                                        gated_callback, &gate};
  h2_pal_timer_t *timer = nullptr;
  assert(h2_pal_timer_create(api, &config, &timer) == H2_PAL_OK);
  {
    std::unique_lock<std::mutex> lock(gate.mutex);
    assert(gate.changed.wait_for(lock, 2s, [&] { return gate.entered; }));
  }
  std::thread destroyer([&] {
    assert(h2_pal_timer_destroy(api, timer) == H2_PAL_OK);
    std::lock_guard<std::mutex> lock(gate.mutex);
    gate.finished = true;
    gate.changed.notify_all();
  });
  {
    std::unique_lock<std::mutex> lock(gate.mutex);
    assert(!gate.changed.wait_for(lock, 30ms, [&] { return gate.finished; }));
    assert(live_timers() == baseline + 1u);
    gate.release = true;
    gate.changed.notify_all();
  }
  destroyer.join();
  assert(live_timers() == baseline);
}

void test_callback_control_and_churn() {
  const size_t baseline = live_timers();
  for (int mode = 0; mode < 2; ++mode) {
    Calls calls;
    calls.self_stop = mode == 0;
    calls.self_destroy = mode == 1;
    const h2_pal_timer_config_t config = {
        "self-control", 1u, H2_PAL_TIMER_FLAG_REPEAT, callback, &calls};
    h2_pal_timer_t *timer = nullptr;
    assert(h2_pal_timer_create(api, &config, &timer) == H2_PAL_OK);
    assert(h2_pal_timer_start(api, timer) == H2_PAL_OK);
    wait_for(calls, 1u);
    assert(calls.result == H2_PAL_OK);
    quiet(calls, 1u);
    if (!calls.self_destroy)
      assert(h2_pal_timer_destroy(api, timer) == H2_PAL_OK);
    wait_for_reclamation(baseline);
  }
  for (int i = 0; i < 100; ++i) {
    Calls calls;
    const h2_pal_timer_config_t config = {
        "churn", 1u, H2_PAL_TIMER_FLAG_AUTO_START, callback, &calls};
    h2_pal_timer_t *timer = nullptr;
    assert(h2_pal_timer_create(api, &config, &timer) == H2_PAL_OK);
    assert(h2_pal_timer_destroy(api, timer) == H2_PAL_OK);
    assert(live_timers() == baseline);
  }
}
} // namespace

int main() {
  alarm(30u);
  api = h2_desktop_platform_timer_api();
  test_validation();
  test_one_shot();
  test_repeat_and_generation();
  test_external_stop_waits();
  test_external_destroy_waits();
  test_callback_control_and_churn();
  return 0;
}
