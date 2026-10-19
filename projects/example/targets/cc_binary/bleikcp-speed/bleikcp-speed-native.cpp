#include "bleikcp-speed-native.h"

#include "h2_bleikcp_speed.h"
#include "h2/pal/h2_pal_unsupported.h"

#include "h2_atomic.h"
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
  h2_atomic_bool_t stop = {};
  h2_atomic_bool_t done = {};
  h2_atomic_int_t result = {};
};

int ready(void *) {
  return H2_PAL_OK;
}

int advertising_noop(void *) {
  return H2_PAL_OK;
}

bool should_stop(void *user) {
  const auto *state = static_cast<RunState *>(user);
  return state == nullptr || h2_atomic_bool_load(&state->stop, H2_ATOMIC_ACQUIRE);
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
  h2_atomic_int_store(&state->result, h2_bleikcp_speed_run(runtime, &config),
                      H2_ATOMIC_RELEASE);
  h2_atomic_bool_store(&state->done, true, H2_ATOMIC_RELEASE);
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
  if (h2_atomic_bool_init(&state.stop, false) != H2_ATOMIC_OK ||
      h2_atomic_bool_init(&state.done, false) != H2_ATOMIC_OK ||
      h2_atomic_int_init(&state.result, H2_PAL_ERR_INVALID_STATE) != H2_ATOMIC_OK) {
    h2_atomic_bool_destroy(&state.stop);
    h2_atomic_bool_destroy(&state.done);
    h2_atomic_int_destroy(&state.result);
    h2_runtime_deinit(runtime);
    (void)h2_pal_display_close(display);
    return 1;
  }
  auto destroy_state = [&state]() {
    h2_atomic_bool_destroy(&state.stop);
    h2_atomic_bool_destroy(&state.done);
    h2_atomic_int_destroy(&state.result);
  };
  std::thread worker;
  try {
    worker = std::thread(worker_main, runtime, client, &state);
  } catch (...) {
    std::fprintf(stderr, "desktop %s: worker start failed\n",
                 layout.app_name);
    h2_runtime_deinit(runtime);
    (void)h2_pal_display_close(display);
    destroy_state();
    return 1;
  }
  while (!h2_atomic_bool_load(&state.done, H2_ATOMIC_ACQUIRE)) {
    if (signal_stop_requested != 0 ||
        poll_events(&display_provider) != 0) {
      h2_atomic_bool_store(&state.stop, true, H2_ATOMIC_RELEASE);
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
  h2_atomic_bool_store(&state.stop, true, H2_ATOMIC_RELEASE);
  worker.join();
  const int result = h2_atomic_int_load(&state.result, H2_ATOMIC_ACQUIRE);
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
  destroy_state();
  return result == H2_PAL_OK && stop_result == H2_PAL_OK ? 0 : 1;
}

} // namespace h2::desktop
