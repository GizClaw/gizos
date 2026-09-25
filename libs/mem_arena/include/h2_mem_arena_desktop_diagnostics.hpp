#ifndef H2_MEM_ARENA_DESKTOP_DIAGNOSTICS_HPP
#define H2_MEM_ARENA_DESKTOP_DIAGNOSTICS_HPP

#include "h2_mem_arena.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace h2::mem_arena {

/* Optional Host-only diagnostics. The caller chooses all product names,
 * budgets and output policy. Tag names remain borrowed until destruction;
 * every borrower must be stopped and joined before destruction. */
struct DesktopDiagnosticsConfig {
  /* A malloc-backed region and the arena's two size classes. */
  const char *arena_name = nullptr;
  size_t reserved_bytes = 0;
  size_t small_request_max = 0;
  size_t small_pool_bytes = 0;
  /* Unique borrowed tag names. Index zero receives unknown owners. */
  const char *const *tags = nullptr;
  size_t tag_count = 0;
  /* The stack tag skips pattern-fill and content scans. */
  size_t stack_tag_index = 0;
  /* Trace changes the initial contents of non-stack allocations to 0xd3. */
  bool trace = false;
  /* NULL report_path uses /tmp/arena-diagnostics-<pid>.log. */
  const char *report_path = nullptr;
  const char *console_prefix = nullptr;
  uintptr_t symbol_anchor = 0;
  const char *symbol_anchor_name = nullptr;
  /* Explicit finite metadata budgets. Overflow degrades trace precision,
   * never the underlying business alloc/free/realloc result. */
  size_t block_capacity = 65536;
  size_t site_capacity = 512;
  size_t peak_capacity = 256;
  size_t duplicate_capacity = 1024;
  size_t report_queue_capacity = 16;
  size_t top_count = 32;
};

class DesktopArenaDiagnostics {
public:
  using Stats = h2_mem_arena_stats_t;

  /* fallback is borrowed until destroy. Invalid configuration or arena
   * creation failure leaves mem()/tagged() NULL and emits no exit report. */
  DesktopArenaDiagnostics(const DesktopDiagnosticsConfig &config,
                          const h2_pal_mem_api_t *fallback);
  ~DesktopArenaDiagnostics();
  DesktopArenaDiagnostics(const DesktopArenaDiagnostics &) = delete;
  DesktopArenaDiagnostics &operator=(const DesktopArenaDiagnostics &) = delete;

  /* Borrowed Memory PALs; do not use them after destruction. Any tag view may
   * free/realloc a block, including when bounded metadata has overflowed. */
  const h2_pal_mem_api_t *mem() const;
  const h2_pal_mem_api_t *tagged(const char *owner) const;
  Stats stats();
  /* report snapshots allocator metadata synchronously and queues bounded
   * content work. flush waits for all queued reports; poll requests deferred
   * peak samples no more than once per second. Call none after borrower join
   * has begun unless the caller serializes its own lifecycle. */
  void report(const char *phase);
  void flush();
  void poll();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace h2::mem_arena
#endif
