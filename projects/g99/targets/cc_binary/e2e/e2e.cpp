// Desktop G99 product E2E. Opens the same Desktop platform as the Main
// launcher, runs the suite on a worker while this thread pumps SDL events,
// and exits with the suite's exit code (0 pass, 1 failing cases, 2 incomplete).
//
//   H2_E2E_SELECT  optional comma-separated selection, see the suite README.
//
// Under Bazel the preferences live in TEST_TMPDIR and SDL_VIDEODRIVER=dummy
// keeps the run headless; run the binary directly to watch it in a window.

#include "g99_desktop.h"
#include "h2_g99_e2e.h"
#include "layout_config.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {

struct Run {
  h2_g99_e2e_target_t target = {};
  h2_g99_e2e_summary_t summary = {};
  h2_pal_result_t result = H2_PAL_OK;
  std::atomic<bool> abort{false};
  std::atomic<bool> done{false};
};

int should_abort(void* user) {
  return static_cast<Run*>(user)->abort.load() ? 1 : 0;
}

void run_suite(Run* run) {
  run->result = h2_g99_e2e_run(&run->target, &run->summary);
  run->done.store(true);
}

}  // namespace

int main() {
  constexpr h2::desktop::Layout layout = {
      h2_desktop_layout::app_name,        h2_desktop_layout::title,
      h2_desktop_layout::width,           h2_desktop_layout::height,
      h2_desktop_layout::mounts,          h2_desktop_layout::mount_count,
      h2_desktop_layout::peripherals,     h2_desktop_layout::peripheral_count,
      h2_desktop_layout::normalized_json,
  };
  h2::desktop::g99::use_test_storage_root();
  h2::desktop::g99::Platform platform;
  if (platform.open(layout) != H2_PAL_OK) {
    std::fprintf(stderr, "g99 e2e: platform configuration failed\n");
    return 2;
  }
  Run run;
  run.target.runtime = platform.runtime();
  run.target.board = layout.app_name;
  run.target.target = "desktop";
  run.target.display_width = static_cast<int32_t>(layout.width);
  run.target.display_height = static_cast<int32_t>(layout.height);
  // The layout's simulated station serves the public test network.
  run.target.capabilities = H2_G99_E2E_CAP_WIFI_TEST_NETWORK;
  run.target.select = std::getenv("H2_E2E_SELECT");
  run.target.should_abort = should_abort;
  run.target.abort_user = &run;
  std::thread worker;
  try {
    worker = std::thread(run_suite, &run);
  } catch (...) {
    return 2;
  }
  while (!run.done.load()) {
    // Closing the window aborts the run; the suite still stops Main.
    if (h2::desktop::poll_events(platform.display()) != 0)
      run.abort.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
  }
  worker.join();
  const int cleanup = platform.close();
  if (run.result != H2_PAL_OK || cleanup != H2_PAL_OK) return 2;
  return h2_g99_e2e_exit_code(&run.summary);
}
