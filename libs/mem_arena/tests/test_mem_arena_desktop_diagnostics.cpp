#include "h2_mem_arena_desktop_diagnostics.hpp"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

namespace {
constexpr size_t kReservedBytes = 8u * 1024u * 1024u;
constexpr size_t kSmallRequestMax = 32u * 1024u;
constexpr size_t kSmallPoolBytes = kReservedBytes / 6u / 8u * 8u;
const char *const kTags[] = {"other", "service", "image", "task_stack"};
h2::mem_arena::DesktopDiagnosticsConfig test_config() {
  h2::mem_arena::DesktopDiagnosticsConfig config;
  config.arena_name = "desktop-diagnostics-test";
  config.reserved_bytes = kReservedBytes;
  config.small_request_max = kSmallRequestMax;
  config.small_pool_bytes = kSmallPoolBytes;
  config.tags = kTags;
  config.tag_count = 4;
  config.stack_tag_index = 3;
  return config;
}
struct Heap {
  bool fail = false;
  size_t blocks = 0;
};
void *alloc(void *user, size_t bytes) {
  auto *heap = static_cast<Heap *>(user);
  if (heap->fail)
    return nullptr;
  void *p = std::malloc(bytes);
  if (p != nullptr)
    ++heap->blocks;
  return p;
}
void *realloc(void *user, void *p, size_t bytes) {
  auto *heap = static_cast<Heap *>(user);
  if (heap->fail)
    return nullptr;
  return std::realloc(p, bytes);
}
void free(void *user, void *p) {
  if (p == nullptr)
    return;
  auto *heap = static_cast<Heap *>(user);
  assert(heap->blocks > 0);
  --heap->blocks;
  std::free(p);
}
const h2_pal_mem_vtable_t vtable = {alloc, realloc, free};
} // namespace

int main() {
  using h2::mem_arena::DesktopArenaDiagnostics;
  Heap heap;
  const h2_pal_mem_api_t inner = {&heap, &vtable};
  {
    DesktopArenaDiagnostics arena(test_config(), &inner);
    const auto *mem = arena.mem();
    assert(mem != nullptr && arena.stats().reserved_bytes == kReservedBytes);
    assert(heap.blocks == 0); // Reservation is malloc, only fallback uses Runtime.
    assert(arena.stats().small.reserved_bytes == kSmallPoolBytes);
    assert(arena.stats().large.reserved_bytes ==
           kReservedBytes - kSmallPoolBytes);
    void *p = h2_pal_mem_alloc(mem, 101);
    std::memset(p, 0xa5, 101);
    void *guard = h2_pal_mem_alloc(mem, 37);
    p = h2_pal_mem_realloc(mem, p, 8097);
    assert(reinterpret_cast<uintptr_t>(p) % alignof(std::max_align_t) == 0);
    for (size_t i = 0; i < 101; ++i)
      assert(static_cast<uint8_t *>(p)[i] == 0xa5);
    heap.fail = true;
    assert(h2_pal_mem_realloc(mem, p, kReservedBytes) == nullptr);
    assert(arena.stats().small.live_bytes == 8134 && arena.stats().fallback_live_bytes == 0);
    heap.fail = false;
    p = h2_pal_mem_realloc(mem, p, kReservedBytes);
    assert(arena.stats().small.live_bytes == 37);
    assert(arena.stats().fallback_live_bytes == kReservedBytes);
    p = h2_pal_mem_realloc(mem, p, 2 * kReservedBytes);
    p = h2_pal_mem_realloc(mem, p, 201);
    for (size_t i = 0; i < 101; ++i)
      assert(static_cast<uint8_t *>(p)[i] == 0xa5);
    assert(arena.stats().small.live_bytes == 238 && arena.stats().fallback_live_bytes == 0);
    assert(arena.stats().small.peak_bytes == 8134);
    assert(arena.stats().large.fallback_count == 3);
    assert(arena.stats().large.fallback_bytes == 4 * kReservedBytes);
    h2_pal_mem_free(mem, guard);
    assert(h2_pal_mem_realloc(mem, p, 0) == nullptr);
    assert(h2_pal_mem_alloc(mem, SIZE_MAX) == nullptr);
    // The exact boundary belongs to small; realloc across it preserves data.
    p = h2_pal_mem_alloc(mem, kSmallRequestMax);
    assert(p != nullptr);
    std::memset(p, 0xa5, kSmallRequestMax);
    assert(arena.stats().small.live_bytes == kSmallRequestMax);
    p = h2_pal_mem_realloc(mem, p, kSmallRequestMax + 1u);
    assert(p != nullptr && arena.stats().small.live_bytes == 0u);
    assert(arena.stats().large.live_bytes == kSmallRequestMax + 1u);
    for (size_t i = 0; i < kSmallRequestMax; ++i)
      assert(static_cast<uint8_t *>(p)[i] == 0xa5);
    h2_pal_mem_free(mem, p);

    // Exhaust both pools; GizOS borrows from the large pool before fallback.
    std::vector<void *> small;
    do {
      p = h2_pal_mem_alloc(mem, kSmallRequestMax);
      assert(p != nullptr);
      small.push_back(p);
      assert(small.size() < kReservedBytes / kSmallRequestMax + 16u);
    } while (arena.stats().small.fallback_count == 0u);
    assert(arena.stats().small.fallback_live_bytes == kSmallRequestMax);
    assert(arena.stats().large.borrowed_count > 0u);
    const size_t large_before = arena.stats().large.live_bytes;
    p = h2_pal_mem_alloc(mem, 1024u * 1024u);
    assert(p != nullptr && arena.stats().large.live_bytes >= large_before);
    h2_pal_mem_free(mem, p);
    for (void *block : small)
      h2_pal_mem_free(mem, block);
    assert(heap.blocks == 0u);

    std::vector<std::thread> workers;
    for (unsigned t = 0; t < 4; ++t)
      workers.emplace_back([mem] {
        for (unsigned i = 1; i < 1000; ++i) {
          void *block = h2_pal_mem_alloc(mem, i);
          assert(block != nullptr);
          std::memset(block, 0x3c, i);
          block = h2_pal_mem_realloc(mem, block, i * 2u);
          for (size_t j = 0; j < i; ++j)
            assert(static_cast<uint8_t *>(block)[j] == 0x3c);
          h2_pal_mem_free(mem, block);
        }
      });
    for (auto &worker : workers)
      worker.join();
    assert(arena.stats().small.live_bytes == 0 && arena.stats().fallback_live_bytes == 0);
  }
  assert(heap.blocks == 0);
  heap.fail = true;
  {
    DesktopArenaDiagnostics arena(test_config(), &inner);
    // A failing Runtime heap cannot prevent the independent reservation.
    assert(arena.mem() != nullptr);
    void *p = h2_pal_mem_alloc(arena.mem(), 64u);
    assert(p != nullptr);
    assert(h2_pal_mem_alloc(arena.mem(), kReservedBytes) == nullptr);
    assert(arena.stats().large.fallback_count == 1u);
    h2_pal_mem_free(arena.mem(), p);
  }
  assert(heap.blocks == 0);
  {
    auto invalid = test_config();
    invalid.reserved_bytes = 0;
    DesktopArenaDiagnostics unavailable(invalid, &inner);
    assert(unavailable.mem() == nullptr);
    unavailable.report("desktop_exit");
    assert(unavailable.stats().reserved_bytes == 0);
  }
  {
    auto unavailable_config = test_config();
    unavailable_config.reserved_bytes = SIZE_MAX;
    DesktopArenaDiagnostics unavailable(unavailable_config, &inner);
    assert(unavailable.mem() == nullptr);
    unavailable.report("desktop_exit");
  }
  const char *tmp = std::getenv("TEST_TMPDIR");
  const std::string path = std::string(tmp != nullptr ? tmp : "/tmp") +
                           "/arena-overflow-" + std::to_string(getpid()) +
                           ".log";
  {
    auto config = test_config();
    config.trace = true;
    config.block_capacity = 2;
    config.site_capacity = 2;
    config.peak_capacity = 2;
    config.duplicate_capacity = 1;
    config.report_queue_capacity = 1;
    config.report_path = path.c_str();
    DesktopArenaDiagnostics arena(config, &inner);
    const auto *service = arena.tagged("service");
    const auto *image = arena.tagged("image");
    std::vector<void *> blocks;
    for (unsigned i = 0; i < 32; ++i) {
      void *block = h2_pal_mem_alloc(i % 2 ? service : image, 64);
      assert(block != nullptr);
      std::memset(block, 0x5a, 64);
      blocks.push_back(block);
    }
    assert(h2_pal_mem_realloc(service, blocks[31], SIZE_MAX) == nullptr);
    for (unsigned i = 0; i < 32; ++i) {
      void *block = h2_pal_mem_realloc(i % 2 ? image : service, blocks[i], 128);
      assert(block != nullptr);
      for (size_t j = 0; j < 64; ++j)
        assert(static_cast<unsigned char *>(block)[j] == 0x5a);
      h2_pal_mem_free(i % 2 ? service : image, block);
    }
    std::vector<std::thread> overflow_workers;
    for (unsigned t = 0; t < 4; ++t)
      overflow_workers.emplace_back([service, image] {
        for (unsigned i = 0; i < 100; ++i) {
          void *block = h2_pal_mem_alloc(service, 64);
          assert(block != nullptr);
          std::memset(block, 0x4c, 64);
          block = h2_pal_mem_realloc(image, block, 96);
          assert(block != nullptr);
          for (size_t j = 0; j < 64; ++j)
            assert(static_cast<unsigned char *>(block)[j] == 0x4c);
          h2_pal_mem_free(image, block);
        }
      });
    for (auto &worker : overflow_workers)
      worker.join();
    for (unsigned i = 0; i < 8; ++i)
      arena.report("case_end:overflow");
    arena.flush();
    assert(arena.stats().small.live_bytes == 0);
  }
  {
    std::ifstream report(path);
    assert(report.good());
    const std::string text{std::istreambuf_iterator<char>(report),
                           std::istreambuf_iterator<char>()};
    bool saw_overflow = false;
    size_t snapshots = 0;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
      if (line.rfind("SNAPSHOT ", 0) == 0)
        ++snapshots;
      if (line.rfind("OVERFLOW ", 0) == 0) {
        const size_t key = line.find(" blocks=");
        assert(key != std::string::npos);
        saw_overflow |= std::stoull(line.substr(key + 8)) > 0;
      }
    }
    assert(saw_overflow && snapshots >= 9);
    assert(text.find("END live=0") != std::string::npos);
  }
  assert(std::remove(path.c_str()) == 0);
}
