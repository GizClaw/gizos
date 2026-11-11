#ifndef H2_G99_DESKTOP_H
#define H2_G99_DESKTOP_H

// Desktop composition shared by the G99 Main and E2E launchers: the layout's
// peripherals and simulated Wi-Fi, the logical key mapping, the Desktop
// providers and one Runtime with input started and the display open.

#include "h2_desktop_app_support.h"
#include "h2_runtime.h"

namespace h2::desktop::g99 {

class Platform {
 public:
  Platform() = default;
  Platform(const Platform&) = delete;
  Platform& operator=(const Platform&) = delete;
  // Closes the display and destroys Runtime before the providers.
  ~Platform();

  // Opens every provider and the Runtime once; returns H2_PAL_OK or the first
  // failure, leaving whatever opened for the destructor.
  int open(const Layout& layout);
  // Idempotent; call after every Runtime user stopped. Failure retains
  // ownership.
  int close();

  h2_runtime_t* runtime() const { return runtime_; }
  OwnedDisplay* display() { return &display_; }

 private:
  OwnedPreference preference_;
  OwnedAudio audio_;
  OwnedDisplay display_;
  h2_runtime_t* runtime_ = nullptr;
  bool display_open_ = false;
};

// Points H2_DESKTOP_STORAGE_ROOT at TEST_TMPDIR when a Bazel test left it
// unset, so tests never write preferences into the runfiles tree.
void use_test_storage_root();

}  // namespace h2::desktop::g99

#endif  // H2_G99_DESKTOP_H
