#ifndef H2_E2E_H2LOADER_SERIAL_CANCELLATION_H
#define H2_E2E_H2LOADER_SERIAL_CANCELLATION_H

#include <atomic>
#include <csignal>

namespace h2::e2e::loader_serial {

/* One process runs one operation. Signal handlers only set a lock-free flag;
 * ordinary code observes cancellation and closes the protocol/session owner. */
class Cancellation final {
 public:
  static bool install() {
    return std::signal(SIGINT, handle_signal) != SIG_ERR &&
           std::signal(SIGTERM, handle_signal) != SIG_ERR;
  }
  static int requested(void *) {
    return requested_.load(std::memory_order_relaxed) != 0;
  }

 private:
  static void handle_signal(int) noexcept {
    requested_.store(1, std::memory_order_relaxed);
  }
  inline static std::atomic<std::sig_atomic_t> requested_{0};
};

static_assert(std::atomic<std::sig_atomic_t>::is_always_lock_free,
              "CLI signal cancellation requires a lock-free flag");

}  // namespace h2::e2e::loader_serial
#endif
