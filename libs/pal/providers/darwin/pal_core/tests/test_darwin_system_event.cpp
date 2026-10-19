#include "h2_darwin_platform.h"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace {
using namespace std::chrono_literals;
const h2_pal_system_event_api_t *api;
constexpr auto type = H2_PAL_SYSTEM_EVENT_TYPE_GPIO_IRQ;
constexpr auto nested_type = H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_CONNECTED;

void post(uint32_t source = 0u, h2_pal_system_event_type_t event_type = type) {
  const h2_pal_system_event_t event = {event_type, source, 0u, nullptr, 0u};
  assert(h2_pal_system_event_post(api, &event, 100u) == H2_PAL_OK);
}

struct Self {
  h2_pal_system_event_subscription_t *subscription = nullptr;
  int calls = 0;
};
int self_unsubscribe(void *user, const h2_pal_system_event_t *) {
  auto *state = static_cast<Self *>(user);
  ++state->calls;
  h2_pal_system_event_unsubscribe(api, state->subscription);
  return H2_PAL_OK;
}
void test_self_and_reuse() {
  for (int cycle = 0; cycle < 100; ++cycle) {
    Self state;
    assert(h2_pal_system_event_subscribe(api, type, self_unsubscribe, &state,
                                         &state.subscription) == H2_PAL_OK);
    post();
    post();
    assert(state.calls == 1);
  }
}

struct Gate {
  std::mutex mutex;
  std::condition_variable changed;
  h2_pal_system_event_subscription_t *subscription = nullptr;
  size_t entered = 0u;
  bool release = false;
  bool self_returned = false;
  bool unsubscribe_started = false;
  bool unsubscribe_finished = false;
};
int gated_handler(void *user, const h2_pal_system_event_t *event) {
  auto *gate = static_cast<Gate *>(user);
  std::unique_lock<std::mutex> lock(gate->mutex);
  if (event->source_id == 2u) {
    lock.unlock();
    h2_pal_system_event_unsubscribe(api, gate->subscription);
    lock.lock();
    gate->self_returned = true;
    gate->changed.notify_all();
    return H2_PAL_OK;
  }
  ++gate->entered;
  gate->changed.notify_all();
  assert(gate->changed.wait_for(lock, 2s, [&] { return gate->release; }));
  return H2_PAL_OK;
}
void test_self_does_not_wait_for_other_inflight_call() {
  Gate gate;
  assert(h2_pal_system_event_subscribe(api, type, gated_handler, &gate,
                                       &gate.subscription) == H2_PAL_OK);
  std::thread first([] { post(1u); });
  {
    std::unique_lock<std::mutex> lock(gate.mutex);
    assert(gate.changed.wait_for(lock, 2s, [&] { return gate.entered == 1u; }));
  }
  post(2u); // Must return while the first invocation still borrows gate.
  post(3u); // Admission has stopped; this cannot enter gated_handler.
  {
    std::lock_guard<std::mutex> lock(gate.mutex);
    assert(gate.self_returned && gate.entered == 1u);
    gate.release = true;
    gate.changed.notify_all();
  }
  first.join(); // User storage stays live through the already-running call.
}

void test_external_waits_for_all_calls() {
  Gate gate;
  assert(h2_pal_system_event_subscribe(api, type, gated_handler, &gate,
                                       &gate.subscription) == H2_PAL_OK);
  std::thread first([] { post(1u); });
  std::thread second([] { post(1u); });
  {
    std::unique_lock<std::mutex> lock(gate.mutex);
    assert(gate.changed.wait_for(lock, 2s, [&] { return gate.entered == 2u; }));
  }
  std::thread unsubscribing([&] {
    {
      std::lock_guard<std::mutex> lock(gate.mutex);
      gate.unsubscribe_started = true;
      gate.changed.notify_all();
    }
    h2_pal_system_event_unsubscribe(api, gate.subscription);
    std::lock_guard<std::mutex> lock(gate.mutex);
    gate.unsubscribe_finished = true;
    gate.changed.notify_all();
  });
  {
    std::unique_lock<std::mutex> lock(gate.mutex);
    assert(gate.changed.wait_for(lock, 2s,
                                 [&] { return gate.unsubscribe_started; }));
    assert(!gate.changed.wait_for(lock, 30ms,
                                  [&] { return gate.unsubscribe_finished; }));
    gate.release = true;
    gate.changed.notify_all();
  }
  first.join();
  second.join();
  unsubscribing.join();
  post(3u);
  assert(gate.entered == 2u && gate.unsubscribe_finished);
}

void test_deinit_drains_unsubscribe_waiters() {
  Gate gate;
  assert(h2_pal_system_event_subscribe(api, type, gated_handler, &gate,
                                       &gate.subscription) == H2_PAL_OK);
  std::thread posting([] { post(1u); });
  {
    std::unique_lock<std::mutex> lock(gate.mutex);
    assert(gate.changed.wait_for(lock, 2s, [&] { return gate.entered == 1u; }));
  }
  std::thread unsubscribing([&] {
    {
      std::lock_guard<std::mutex> lock(gate.mutex);
      gate.unsubscribe_started = true;
      gate.changed.notify_all();
    }
    h2_pal_system_event_unsubscribe(api, gate.subscription);
    std::lock_guard<std::mutex> lock(gate.mutex);
    gate.unsubscribe_finished = true;
    gate.changed.notify_all();
  });
  {
    std::unique_lock<std::mutex> lock(gate.mutex);
    assert(gate.changed.wait_for(lock, 2s,
                                 [&] { return gate.unsubscribe_started; }));
    assert(!gate.changed.wait_for(lock, 30ms,
                                  [&] { return gate.unsubscribe_finished; }));
  }
  bool deinit_finished = false;
  std::thread deinit([&] {
    h2_pal_system_event_deinit(api);
    std::lock_guard<std::mutex> lock(gate.mutex);
    deinit_finished = true;
    gate.changed.notify_all();
  });
  {
    std::unique_lock<std::mutex> lock(gate.mutex);
    assert(!gate.changed.wait_for(lock, 30ms, [&] { return deinit_finished; }));
    gate.release = true;
    gate.changed.notify_all();
  }
  posting.join();
  unsubscribing.join();
  deinit.join();
  assert(deinit_finished && gate.unsubscribe_finished);
  assert(h2_pal_system_event_init(api) == H2_PAL_OK);
  test_self_and_reuse();
}

struct Replace {
  h2_pal_system_event_subscription_t *first = nullptr;
  h2_pal_system_event_subscription_t *second = nullptr;
  h2_pal_system_event_subscription_t *replacement = nullptr;
  int first_calls = 0;
  int second_calls = 0;
  int replacement_calls = 0;
};
int replacement_handler(void *user, const h2_pal_system_event_t *) {
  ++static_cast<Replace *>(user)->replacement_calls;
  return H2_PAL_OK;
}
int second_handler(void *user, const h2_pal_system_event_t *) {
  ++static_cast<Replace *>(user)->second_calls;
  return H2_PAL_OK;
}
int replace_later_listener(void *user, const h2_pal_system_event_t *) {
  auto *state = static_cast<Replace *>(user);
  if (++state->first_calls == 1) {
    h2_pal_system_event_unsubscribe(api, state->second);
    assert(h2_pal_system_event_subscribe(api, type, replacement_handler, state,
                                         &state->replacement) == H2_PAL_OK);
  }
  return H2_PAL_OK;
}
void test_callback_can_cancel_later_snapshot_member() {
  Replace state;
  assert(h2_pal_system_event_subscribe(api, type, replace_later_listener,
                                       &state, &state.first) == H2_PAL_OK);
  assert(h2_pal_system_event_subscribe(api, type, second_handler, &state,
                                       &state.second) == H2_PAL_OK);
  post();
  assert(state.first_calls == 1 && state.second_calls == 0 &&
         state.replacement_calls == 0);
  post();
  assert(state.first_calls == 2 && state.second_calls == 0 &&
         state.replacement_calls == 1);
  h2_pal_system_event_unsubscribe(api, state.first);
  h2_pal_system_event_unsubscribe(api, state.replacement);
}

struct Nested {
  h2_pal_system_event_subscription_t *outer = nullptr;
  h2_pal_system_event_subscription_t *inner = nullptr;
  int calls = 0;
};
int outer_handler(void *user, const h2_pal_system_event_t *) {
  ++static_cast<Nested *>(user)->calls;
  post(0u, nested_type);
  return H2_PAL_OK;
}
int inner_handler(void *user, const h2_pal_system_event_t *) {
  h2_pal_system_event_unsubscribe(api, static_cast<Nested *>(user)->outer);
  return H2_PAL_OK;
}
void test_nested_callback_can_cancel_ancestor() {
  Nested state;
  assert(h2_pal_system_event_subscribe(api, type, outer_handler, &state,
                                       &state.outer) == H2_PAL_OK);
  assert(h2_pal_system_event_subscribe(api, nested_type, inner_handler, &state,
                                       &state.inner) == H2_PAL_OK);
  post();
  post();
  assert(state.calls == 1);
  h2_pal_system_event_unsubscribe(api, state.inner);
}
} // namespace

int main() {
  alarm(30u);
  api = h2_darwin_system_event_api();
  assert(h2_pal_system_event_init(api) == H2_PAL_OK);
  test_self_and_reuse();
  test_self_does_not_wait_for_other_inflight_call();
  test_external_waits_for_all_calls();
  test_deinit_drains_unsubscribe_waiters();
  test_callback_can_cancel_later_snapshot_member();
  test_nested_callback_can_cancel_ancestor();
  h2_pal_system_event_deinit(api);
  assert(h2_pal_system_event_init(api) == H2_PAL_OK);
  test_self_and_reuse();
  h2_pal_system_event_deinit(api);
  return 0;
}
