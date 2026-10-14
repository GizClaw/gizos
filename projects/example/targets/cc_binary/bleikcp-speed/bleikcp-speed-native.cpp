#include "bleikcp-speed-native.h"
#include "h2_desktop_application_host.hpp"

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

} // namespace

static h2_pal_result_t configure(void *, h2_runtime_config_t *config) {
  config->chip = "macos";
  config->ble_host = corebluetooth_api();
  config->video_decoder = h2_pal_unsupported_video_decoder_api();
  return H2_PAL_OK;
}
static int advertising_noop(void *) { return H2_PAL_OK; }
static bool should_stop(void *user) {
  return signal_stop_requested != 0 || h2_application_host_should_stop(user);
}
static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *runtime, void *user) {
  h2_bleikcp_speed_config_t config{};
  config.role = user != nullptr ? H2_BLEIKCP_SPEED_ROLE_CLIENT : H2_BLEIKCP_SPEED_ROLE_SERVER;
  config.advertising_type = H2_PAL_BLE_ADV_TYPE_LEGACY;
  config.scan_type = H2_PAL_BLE_SCAN_TYPE_LEGACY;
  if (user != nullptr) {
    config.pause_management_advertising = advertising_noop;
    config.resume_management_advertising = advertising_noop;
  }
  config.ready = h2_application_host_ready;
  config.ready_user = host;
  config.should_stop = should_stop;
  config.stop_user = host;
  int rc = h2_pal_display_open(runtime->display);
  if (rc == H2_PAL_OK) rc = h2_bleikcp_speed_run(runtime, &config);
  int stopped = h2_pal_ble_stop(runtime->ble_host);
  if (stopped != H2_PAL_OK) {
    h2_application_host_retain(host, nullptr, stopped);
    return stopped;
  }
  return rc;
}
int run_bleikcp_speed(const Layout &layout, bool client) {
  signal_stop_requested = 0;
  std::signal(SIGINT, request_stop_from_signal);
  std::signal(SIGTERM, request_stop_from_signal);
  auto platform = application_host_config(layout);
  platform.entry_on_main = 0;
  h2_application_host_config_t app{};
  app.entry = run;
  app.configure = configure;
  app.user = client ? reinterpret_cast<void *>(1) : nullptr;
  h2_application_host_result_t result{};
  return h2_desktop_application_host_run(&platform, &app, &result) == H2_PAL_OK ? 0 : 1;
}

} // namespace h2::desktop
