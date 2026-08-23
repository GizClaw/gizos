#include "bleikcp-speed-native.h"

#include "h2_bleikcp_speed.h"
#include "h2/pal/h2_pal_unsupported.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <thread>

namespace h2::desktop {
namespace {

volatile std::sig_atomic_t signal_stop_requested = 0;

void request_stop_from_signal(int) {
  signal_stop_requested = 1;
}

struct RunState {
  std::atomic<bool> stop = false;
  std::atomic<bool> done = false;
  std::atomic<int> result = H2_PAL_ERR_INVALID_STATE;
};

int ready(void *) {
  return H2_PAL_OK;
}

int advertising_noop(void *) {
  return H2_PAL_OK;
}

bool should_stop(void *user) {
  const auto *state = static_cast<RunState *>(user);
  return state == nullptr || state->stop.load(std::memory_order_acquire);
}

void worker_main(h2_runtime_t *runtime, bool client, RunState *state) {
  h2_bleikcp_speed_config_t config = {};
  config.role = client ? H2_BLEIKCP_SPEED_ROLE_CLIENT
                       : H2_BLEIKCP_SPEED_ROLE_SERVER;
  config.advertising_type = H2_PAL_BLE_ADV_TYPE_LEGACY;
  config.scan_type = H2_PAL_BLE_SCAN_TYPE_LEGACY;
  if (client) {
    config.pause_management_advertising = advertising_noop;
    config.resume_management_advertising = advertising_noop;
  }
  config.ready = ready;
  config.should_stop = should_stop;
  config.stop_user = state;
  state->result.store(h2_bleikcp_speed_run(runtime, &config),
                      std::memory_order_release);
  state->done.store(true, std::memory_order_release);
}

} // namespace

int run_bleikcp_speed(const Layout &layout, bool client) {
  signal_stop_requested = 0;
  std::signal(SIGINT, request_stop_from_signal);
  std::signal(SIGTERM, request_stop_from_signal);
  OwnedDisplay display_provider;
  if (configure_layout(layout) != H2_PAL_OK ||
      open_display(layout, &display_provider) != H2_DISPLAY_OK) {
    std::fprintf(stderr, "desktop %s: invalid display configuration\n",
                 layout.app_name);
    return 1;
  }
  h2_pal_display_t *display = display_provider.display();
  if (h2_pal_display_open(display) != H2_DISPLAY_OK) {
    std::fprintf(stderr, "desktop %s: display open failed\n",
                 layout.app_name);
    return 1;
  }
  h2_runtime_config_t config = runtime_config(nullptr);
  config.chip = "macos";
  config.display = display;
  config.ble_host = corebluetooth_api();
  config.video_decoder = h2_pal_unsupported_video_decoder_api();
  h2_runtime_t *runtime = nullptr;
  const int runtime_result = h2_runtime_init(&config, &runtime);
  if (runtime_result != H2_PAL_OK || runtime == nullptr) {
    std::fprintf(stderr, "desktop %s: runtime init failed (%d)\n",
                 layout.app_name, runtime_result);
    (void)h2_pal_display_close(display);
    return 1;
  }

  RunState state;
  std::thread worker;
  try {
    worker = std::thread(worker_main, runtime, client, &state);
  } catch (...) {
    std::fprintf(stderr, "desktop %s: worker start failed\n",
                 layout.app_name);
    h2_runtime_deinit(runtime);
    (void)h2_pal_display_close(display);
    return 1;
  }
  while (!state.done.load(std::memory_order_acquire)) {
    if (signal_stop_requested != 0 ||
        poll_events(&display_provider) != 0) {
      state.stop.store(true, std::memory_order_release);
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
  state.stop.store(true, std::memory_order_release);
  worker.join();
  const int result = state.result.load(std::memory_order_acquire);
  const int stop_result = h2_pal_ble_stop(runtime->ble_host);
  if (result != H2_PAL_OK) {
    std::fprintf(stderr, "desktop %s: app failed (%d)\n",
                 layout.app_name, result);
  }
  if (stop_result != H2_PAL_OK) {
    std::fprintf(stderr, "desktop %s: BLE stop failed (%d)\n",
                 layout.app_name, stop_result);
  }
  h2_runtime_deinit(runtime);
  (void)h2_pal_display_close(display);
  return result == H2_PAL_OK && stop_result == H2_PAL_OK ? 0 : 1;
}

} // namespace h2::desktop
