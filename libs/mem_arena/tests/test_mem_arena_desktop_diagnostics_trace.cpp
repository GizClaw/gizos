#include "h2_mem_arena_desktop_diagnostics.hpp"

#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <unistd.h>

using h2::mem_arena::DesktopArenaDiagnostics;
using h2::mem_arena::DesktopDiagnosticsConfig;
namespace {
std::string read(const std::string &path) {
  std::ifstream input(path);
  assert(input.good());
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}
} // namespace
int main() {
  const char *tmp = std::getenv("TEST_TMPDIR");
  const std::string path = std::string(tmp != nullptr ? tmp : "/tmp") +
                           "/arena-trace-" + std::to_string(getpid()) + ".log";
  const char *const tags[] = {"other", "service", "image", "task_stack"};
  DesktopDiagnosticsConfig config;
  config.arena_name = "desktop-diagnostics-trace-test";
  config.reserved_bytes = 8u * 1024u * 1024u;
  config.small_request_max = 32u * 1024u;
  config.small_pool_bytes = config.reserved_bytes / 6u / 8u * 8u;
  config.tags = tags;
  config.tag_count = 4;
  config.stack_tag_index = 3;
  config.trace = true;
  config.report_path = path.c_str();
  {
    DesktopArenaDiagnostics arena(config, nullptr);
    const auto *home = arena.tagged("service");
    const auto *image = arena.tagged("image");
    const auto *stacks = arena.tagged("task_stack");
    void *resident = h2_pal_mem_alloc(home, 1024);
    std::memset(resident, 0x41, 100);
    void *a = h2_pal_mem_alloc(image, 16384);
    void *b = h2_pal_mem_alloc(image, 32768);
    std::memset(a, 0x42, 16384);
    std::memcpy(b, a, 16384);
    void *stack = h2_pal_mem_alloc(stacks, 65536);
    arena.report("case_end:first");
    arena.flush();
    const std::string first = read(path);
    assert(first.find("DUPLICATE ") != std::string::npos);
    assert(first.find("bytes=16384") != std::string::npos);
    assert(first.find("unused=924 used_estimate=100") != std::string::npos);
    assert(first.find("tag=task_stack live=65536") != std::string::npos);
    assert(first.find("scanned=0 unused=0 scan_exempt=1") != std::string::npos);
    // Failed growth preserves owner, pattern, generation and the live peak.
    assert(h2_pal_mem_realloc(home, resident, SIZE_MAX) == nullptr);
    void *growth = h2_pal_mem_alloc(home, 256);
    arena.report("case_end:second");
    // A different view must still free/account by the block's original owner.
    h2_pal_mem_free(arena.mem(), a);
    h2_pal_mem_free(arena.mem(), b);
    h2_pal_mem_free(arena.mem(), stack);
    resident = h2_pal_mem_realloc(arena.mem(), resident, 2048);
    assert(resident != nullptr);
    for (size_t i = 0; i < 100; ++i)
      assert(static_cast<unsigned char *>(resident)[i] == 0x41);
    for (size_t i = 1024; i < 2048; ++i)
      assert(static_cast<unsigned char *>(resident)[i] == 0xd3);
    h2_pal_mem_free(home, resident);
    h2_pal_mem_free(home, growth);
    // Snapshot copying tolerates writes and allocator address reuse.
    // Free/realloc synchronization remains the caller's ordinary allocation
    // ownership.
    std::atomic<bool> done{false};
    std::thread writer([&] {
      for (unsigned i = 0; i < 100; ++i) {
        void *ptr = h2_pal_mem_alloc(image, 16384);
        std::memset(ptr, 0x5a, 16384);
        h2_pal_mem_free(image, ptr);
      }
      done = true;
    });
    do {
      arena.report("concurrent");
      arena.flush();
    } while (!done.load());
    writer.join();
  }
  const std::string result = read(path);
  assert(result.find("resident_same_blocks=1024") != std::string::npos);
  assert(result.find("end_growth=256") != std::string::npos);
  assert(result.find("retired_requested=1024 retired_unused=924") !=
         std::string::npos);
  assert(result.find("freed_requested=2048 freed_unused=1948") !=
         std::string::npos);
  assert(result.find("END live=0") != std::string::npos);
  assert(result.find("RANK category=overhead") != std::string::npos);
  // Trace-off keeps cheap attribution and computes a simultaneous total peak,
  // not the sum of unrelated pool peaks. No content/frame work is enabled.
  config.trace = false;
  {
    DesktopArenaDiagnostics arena(config, nullptr);
    void *small = h2_pal_mem_alloc(arena.mem(), 20000);
    h2_pal_mem_free(arena.mem(), small);
    void *large = h2_pal_mem_alloc(arena.mem(), 40000);
    h2_pal_mem_free(arena.mem(), large);
  }
  const std::string plain = read(path);
  assert(plain.find("trace=0") != std::string::npos);
  assert(plain.find("END live=0 peak=40000") != std::string::npos);
  assert(plain.find("\nBLOCK ") == std::string::npos);
  assert(std::remove(path.c_str()) == 0);
}
