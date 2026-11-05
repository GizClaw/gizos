#include "h2_zero_bk_spi_test_sdk.h"
extern "C" {
#include "h2_zero_bk_spi.h"
}
#include <atomic>
#include <cassert>
#include <chrono>
#include <mutex>
#include <thread>

static std::mutex dma_mutex;
static std::atomic<int> active{0};
static std::atomic<int> collisions{0};
static std::atomic<int> calls{0};
static std::atomic<bool> start{false};
static bool fail_write;

extern "C" int rtos_init_mutex(beken_mutex_t* mutex) {
  *mutex = &dma_mutex;
  return 0;
}
extern "C" int rtos_lock_mutex(beken_mutex_t* mutex) {
  static_cast<std::mutex*>(*mutex)->lock();
  return 0;
}
extern "C" int rtos_unlock_mutex(beken_mutex_t* mutex) {
  static_cast<std::mutex*>(*mutex)->unlock();
  return 0;
}
extern "C" int bk_spi_dma_write_bytes(spi_id_t port, const void*, uint32_t) {
  assert(port <= 1u);
  if (active.fetch_add(1) != 0) ++collisions;
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  --active;
  ++calls;
  return fail_write ? -42 : 0;
}
static void transmit(uint32_t port) {
  const uint8_t pixels[2] = {0, 0};
  while (!start.load()) std::this_thread::yield();
  for (int n = 0; n < 40; ++n)
    assert(h2_zero_bk_spi_dma_write(port, pixels, sizeof(pixels)) == 0);
}
int main() {
  const uint8_t pixels[2] = {0, 0};
  assert(h2_zero_bk_spi_dma_write(0, pixels, 2) != 0);
  assert(h2_zero_bk_spi_init() == 0);
  std::thread lcd(transmit, 0u), leds(transmit, 1u);
  start.store(true);
  lcd.join();
  leds.join();
  assert(collisions == 0 && calls == 80);
  fail_write = true;
  assert(h2_zero_bk_spi_dma_write(0, pixels, 2) == -42);
  fail_write = false;
  assert(h2_zero_bk_spi_dma_write(1, pixels, 2) == 0);
  assert(h2_zero_bk_spi_dma_write(0, nullptr, 2) != 0);
}
