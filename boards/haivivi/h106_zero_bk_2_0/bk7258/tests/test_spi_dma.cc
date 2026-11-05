#include "h2_zero_bk_spi_test_sdk.h"
extern "C" {
#include "h2_zero_bk_spi.h"
}
#include <atomic>
#include <cassert>
#include <chrono>
#include <mutex>
#include <thread>

static std::mutex spi_mutex;
static std::atomic<int> active{0}, collisions{0}, calls{0}, votes{0};
static std::atomic<bool> start{false}, awake{false};
static int write_error, lock_error, acquire_error, release_error, unlock_error;

extern "C" int rtos_init_mutex(beken_mutex_t* mutex) {
  *mutex = &spi_mutex;
  return 0;
}
extern "C" int rtos_lock_mutex(beken_mutex_t* mutex) {
  if (lock_error) return lock_error;
  static_cast<std::mutex*>(*mutex)->lock();
  return 0;
}
extern "C" int rtos_unlock_mutex(beken_mutex_t* mutex) {
  static_cast<std::mutex*>(*mutex)->unlock();
  return unlock_error;
}
extern "C" int bk_pm_module_vote_sleep_ctrl(pm_sleep_module_name_e module,
                                            uint32_t sleep_state,
                                            uint32_t sleep_time) {
  assert(module == PM_SLEEP_MODULE_NAME_SPI_1 && sleep_time == 0u);
  ++votes;
  if (!sleep_state) {
    if (acquire_error) return acquire_error;
    awake.store(true);
  } else {
    assert(active.load() == 0);
    if (release_error) return release_error;
    assert(awake.load());
    awake.store(false);
  }
  return 0;
}
static int write(spi_id_t port) {
  assert(port <= 1u && awake.load());
  if (active.fetch_add(1) != 0) ++collisions;
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  /* A sleep transition here would interrupt the pending FIFO/DMA request. */
  assert(awake.load());
  --active;
  ++calls;
  return write_error;
}
extern "C" int bk_spi_dma_write_bytes(spi_id_t port, const void*, uint32_t) {
  return write(port);
}
extern "C" int bk_spi_write_bytes(spi_id_t port, const void*, uint32_t) {
  return write(port);
}
static void transmit(uint32_t port, bool dma) {
  const uint8_t pixels[2] = {0, 0};
  while (!start.load()) std::this_thread::yield();
  for (int n = 0; n < 40; ++n) {
    const int rc = dma ? h2_zero_bk_spi_dma_write(port, pixels, sizeof(pixels))
                       : h2_zero_bk_spi_write(port, pixels, sizeof(pixels));
    assert(rc == 0);
  }
}
int main() {
  const uint8_t pixels[2] = {0, 0};
  assert(h2_zero_bk_spi_dma_write(0, pixels, 2) != 0);
  assert(h2_zero_bk_spi_write(0, pixels, 2) != 0);
  assert(votes == 0 && calls == 0);
  assert(h2_zero_bk_spi_init() == 0);
  std::thread lcd(transmit, 0u, true), leds(transmit, 1u, true),
      commands(transmit, 0u, false);
  start.store(true);
  lcd.join();
  leds.join();
  commands.join();
  assert(collisions == 0 && calls == 120 && votes == 240 && !awake.load());
  lock_error = -21;
  assert(h2_zero_bk_spi_dma_write(0, pixels, 2) == -21);
  lock_error = 0;
  assert(calls == 120 && votes == 240);
  acquire_error = -31;
  assert(h2_zero_bk_spi_dma_write(0, pixels, 2) == -31);
  acquire_error = 0;
  assert(calls == 120 && votes == 241 && !awake.load());
  write_error = -42;
  assert(h2_zero_bk_spi_dma_write(0, pixels, 2) == -42);
  assert(!awake.load());
  release_error = -32;
  unlock_error = -33;
  assert(h2_zero_bk_spi_write(0, pixels, 2) == -42);
  write_error = 0;
  assert(h2_zero_bk_spi_dma_write(0, pixels, 2) == -32);
  release_error = 0;
  assert(h2_zero_bk_spi_write(0, pixels, 2) == -33);
  unlock_error = 0;
  assert(h2_zero_bk_spi_dma_write(1, pixels, 2) == 0 && !awake.load());
  const int before = votes.load();
  assert(h2_zero_bk_spi_dma_write(0, nullptr, 2) != 0);
  assert(h2_zero_bk_spi_write(0, pixels, 0) != 0);
  assert(votes == before && !awake.load());
}
