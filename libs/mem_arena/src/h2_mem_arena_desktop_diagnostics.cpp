#include "h2_mem_arena_desktop_diagnostics.hpp"
#include "h2_mem_arena_census.h"
#include "h2_desktop_platform.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#elif defined(__linux__)
#include <sys/uio.h>
#endif
#if defined(__APPLE__) || defined(__linux__)
#include <execinfo.h>
#include <unistd.h>
#endif

namespace h2::mem_arena {
namespace {
constexpr size_t kMaxTags = 64;
constexpr size_t kLarge = 16u * 1024u;
constexpr unsigned char kPattern = 0xd3;
using Counts = std::array<size_t, kMaxTags>;
using Frames = std::array<uintptr_t, 8>;
using Clock = std::chrono::steady_clock;

// Kernel copy avoids C++ reads racing with application writes. The allocation
// lock prevents free/realloc during each bounded copy; contents remain a
// best-effort observation, not an atomic application-wide memory image.
bool read_memory(const void *src, void *dst, size_t size) {
#if defined(__APPLE__)
  mach_vm_size_t copied = 0;
  return mach_vm_read_overwrite(mach_task_self(),
                                reinterpret_cast<mach_vm_address_t>(src), size,
                                reinterpret_cast<mach_vm_address_t>(dst),
                                &copied) == KERN_SUCCESS &&
         copied == size;
#elif defined(__linux__)
  iovec local = {dst, size};
  iovec remote = {const_cast<void *>(src), size};
  return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) ==
         static_cast<ssize_t>(size);
#else
  (void)src;
  (void)dst;
  (void)size;
  return false;
#endif
}
size_t untouched_tail(const void *ptr, size_t size) {
  const auto *data = static_cast<const unsigned char *>(ptr);
  size_t end = size;
  while (end != 0 && data[end - 1] == kPattern)
    --end;
  return size - end;
}
std::string token(const char *text) {
  std::string result = text != nullptr ? text : "unknown";
  for (char &ch : result)
    if (ch <= ' ' || ch == '=')
      ch = '_';
  return result;
}
} // namespace

struct DesktopArenaDiagnostics::Impl {
  struct Tag {
    size_t live = 0, peak = 0, count = 0, largest = 0, consumed = 0,
           failures = 0;
    size_t freed_requested = 0, freed_unused = 0;
    size_t end_first = 0, end_last = 0, end_min = 0, end_max = 0;
    size_t unused_max = 0, overhead_peak = 0;
  };
  struct Block {
    void *ptr = nullptr;
    uint64_t id = 0;
    size_t size = 0, consumed = 0;
    unsigned tag = 0, site = 0, pool = 0;
    bool fallback = false;
  };
  struct Site {
    Frames frames{};
    size_t requests = 0, requested = 0, freed_requested = 0, freed_unused = 0;
    size_t realloc_count = 0, growth_bytes = 0, moved = 0;
    size_t live = 0, peak = 0, retired_requested = 0, retired_unused = 0;
    size_t unused_snapshot_max = 0, consumed = 0, overhead_peak = 0;
  };
  struct View {
    Impl *self = nullptr;
    unsigned tag = 0;
    h2_pal_mem_api_t api{};
  };
  struct Peak {
    uint64_t seq = 0;
    size_t live = 0, consumed = 0, fallback = 0;
    Counts tags{}, consumed_tags{};
  };
  struct Sample {
    Block block;
    size_t unused = 0;
    uint64_t hash = 14695981039346656037ull;
    bool valid = false;
  };

  struct Duplicate {
    uint64_t first = 0, second = 0, snapshot = 0;
    size_t bytes = 0;
  };
  std::map<std::pair<uint64_t, uint64_t>, Duplicate> duplicate_candidates;

  struct Snapshot {
    std::string phase;
    std::vector<Block> copy, top;
    std::vector<Peak> history;
    std::vector<Site> new_sites;
    size_t first_site = 0;
    std::array<Tag, kMaxTags> tag_copy{};
    h2_mem_arena_pool_inspection_t pools[2]{};
    h2_mem_arena_stats_t stats{};
    Peak peak_copy;
    Counts census_live{}, census_peak{};
    size_t census_unattributed_live = 0;
    size_t census_block_overflow = 0, census_site_overflow = 0;
    size_t block_overflow = 0, site_overflow = 0, peak_overflow = 0;
    size_t report_backpressure = 0;
    uint64_t seq = 0, allocation_seq = 0;
    size_t total = 0, actual = 0;
    int64_t capture_us = 0;
    Clock::time_point captured_at;
  };

  void *storage = nullptr;
  h2_mem_arena_t *core = nullptr;
  h2_mem_arena_census_t *census = nullptr;
  DesktopDiagnosticsConfig options;
  std::vector<h2_mem_arena_census_tag_config_t> census_tags;
  const char *const *tag_names = nullptr;
  size_t tag_count = 0;
  size_t stack_tag = 0;
  size_t top_count = 0;
  std::string console_prefix;
  std::mutex core_mutex, mutex, report_mutex;
  std::condition_variable report_ready, report_done;
  std::deque<Snapshot> pending_reports;
  std::thread reporter;
  bool reporter_stop = false, reporter_active = false;
  uint64_t snapshot_sequence = 0;
  std::array<View, kMaxTags> views{};
  std::array<Tag, kMaxTags> tags{};
  std::unordered_map<void *, Block> blocks;
  std::map<std::pair<size_t, uint64_t>, void *> by_size;
  std::map<Frames, unsigned> site_ids;
  std::vector<Site> sites;
  size_t reported_sites = 0;
  std::vector<Peak> peaks;
  std::vector<Block> peak_blocks;
  std::unordered_map<uint64_t, Block> residents;
  Peak peak{};
  uint64_t sequence = 0, next_id = 0, case_ends = 0;
  size_t live = 0, consumed = 0, fallback_live = 0;
  size_t polled_peak = 0, consumed_peak = 0, realloc_overlap_upper = 0;
  bool trace = false;
  Clock::time_point polled_at = Clock::now();
  std::ofstream output;
  std::string path;
  size_t block_overflow = 0, site_overflow = 0, peak_overflow = 0;
  size_t duplicate_overflow = 0, report_backpressure = 0;

  const h2_pal_mem_api_t *underlying(unsigned tag) const {
    return census != nullptr ? h2_mem_arena_census_tag_mem(census, tag)
                             : h2_mem_arena_mem(core);
  }

#if defined(__clang__)
  __attribute__((no_thread_safety_analysis))
#endif
  static void lock(void *user) {
    static_cast<std::mutex *>(user)->lock();
  }
#if defined(__clang__)
  __attribute__((no_thread_safety_analysis))
#endif
  static void unlock(void *user) {
    static_cast<std::mutex *>(user)->unlock();
  }

  explicit Impl(const DesktopDiagnosticsConfig &cfg,
                const h2_pal_mem_api_t *fallback)
      : options(cfg), tag_names(cfg.tags), tag_count(cfg.tag_count),
        stack_tag(cfg.stack_tag_index), top_count(cfg.top_count),
        console_prefix(cfg.console_prefix != nullptr ? cfg.console_prefix
                                                       : "H2_ARENA"),
        trace(cfg.trace) {
    if (tag_names == nullptr || tag_count == 0 || tag_count > kMaxTags ||
        stack_tag >= tag_count || cfg.reserved_bytes == 0 ||
        cfg.block_capacity == 0 || cfg.site_capacity < 2 ||
        cfg.peak_capacity == 0 || cfg.duplicate_capacity == 0 ||
        cfg.report_queue_capacity == 0 || top_count == 0) {
      std::fprintf(stderr, "H2_ARENA diagnostics invalid config\n");
      return;
    }
    storage = std::malloc(cfg.reserved_bytes);
    h2_mem_arena_config_t config = {};
    config.name = cfg.arena_name;
    config.block = storage;
    config.block_bytes = cfg.reserved_bytes;
    config.small_request_max = cfg.small_request_max;
    config.small_pool_bytes = cfg.small_pool_bytes;
    config.fallback = fallback;
    config.lock = lock;
    config.unlock = unlock;
    config.lock_user = &core_mutex;
    if (h2_mem_arena_create(&config, &core) != H2_PAL_OK) {
      std::free(storage);
      storage = nullptr;
      std::fprintf(stderr, "%s unavailable allocator=fallback\n",
                   console_prefix.c_str());
      return;
    }
    census_tags.resize(tag_count);
    for (size_t i = 0; i < tag_count; ++i)
      census_tags[i] = {tag_names[i], trace && i != stack_tag};
    h2_mem_arena_census_config_t census_config = {};
    census_config.arena = core;
    census_config.metadata = h2_desktop_platform_default_allocator();
    census_config.sync = h2_desktop_platform_sync_api();
    census_config.tags = census_tags.data();
    census_config.tag_count = tag_count;
    census_config.block_capacity = cfg.block_capacity;
    census_config.site_capacity = cfg.site_capacity;
    census_config.probe_limit = std::min<size_t>(16, std::min(
        cfg.block_capacity, cfg.site_capacity - 1));
    const h2_pal_result_t census_rc =
        h2_mem_arena_census_create(&census_config, &census);
    if (census_rc != H2_PAL_OK)
      std::fprintf(stderr, "%s census_unavailable rc=%d\n",
                   console_prefix.c_str(), census_rc);
    for (size_t i = 0; i < tag_count; ++i) {
      views[i].self = this;
      views[i].tag = i;
      views[i].api = {&views[i], &vtable};
    }
    sites.emplace_back();
    path = cfg.report_path != nullptr && cfg.report_path[0] != '\0'
               ? cfg.report_path
               : std::string("/tmp/arena-diagnostics-") +
                     std::to_string(getpid()) + ".log";
    output.open(path, std::ios::out | std::ios::trunc);
    if (!output)
      std::fprintf(stderr, "H2_ARENA_REPORT error=open path=%s\n",
                   path.c_str());
    else {
      output << "META version=1 trace=" << trace
             << " reserved=" << cfg.reserved_bytes
             << " anchor=" << cfg.symbol_anchor
             << " anchor_symbol="
             << (cfg.symbol_anchor_name != nullptr ? cfg.symbol_anchor_name : "none")
             << " content=concurrent_sample unused=pattern_tail_estimate"
             << " fallback_consumed=lower_bound\n";
      output.flush();
      std::fprintf(stderr, "H2_ARENA_REPORT trace=%d path=%s\n", trace,
                   path.c_str());
    }
    try {
      reporter = std::thread([this] { run_reports(); });
    } catch (...) {
      std::fprintf(stderr,
                   "H2_ARENA_REPORT worker=unavailable mode=synchronous\n");
    }
  }
  ~Impl() {
    if (core == nullptr)
      return;
    snapshot("desktop_exit");
    {
      std::lock_guard<std::mutex> guard(report_mutex);
      reporter_stop = true;
    }
    report_ready.notify_one();
    if (reporter.joinable())
      reporter.join();
    summarize();
    if (h2_mem_arena_census_destroy(census) != H2_PAL_OK) {
      std::fprintf(stderr, "%s census_destroy_failed live_allocations\n",
                   console_prefix.c_str());
      std::abort();
    }
    if (h2_mem_arena_destroy(core) != H2_PAL_OK) {
      std::fprintf(stderr, "%s destroy_failed live_allocations\n",
                   console_prefix.c_str());
      std::abort();
    }
    std::free(storage);
  }

  Frames capture() const {
    Frames frames{};
#if defined(__APPLE__) || defined(__linux__)
    if (trace) {
      void *raw[12] = {};
      const int count = backtrace(raw, 12);
      // Skip capture and the allocator adapter; retain PAL and application
      // caller/outer frames for offline symbolization, including static code.
      for (int i = 2; i < count && i - 2 < static_cast<int>(frames.size()); ++i)
        frames[static_cast<size_t>(i - 2)] =
            reinterpret_cast<uintptr_t>(raw[i]);
    }
#endif
    return frames;
  }
  unsigned site(const Frames &frames) {
    if (!trace)
      return 0;
    const auto found = site_ids.find(frames);
    if (found != site_ids.end())
      return found->second;
    if (sites.size() >= options.site_capacity) {
      ++site_overflow;
      return 0;
    }
    const unsigned id = static_cast<unsigned>(sites.size());
    site_ids.emplace(frames, id);
    sites.push_back(Site{});
    sites.back().frames = frames;
    return id;
  }
  void remove(const Block &b) {
    tags[b.tag].live -= b.size;
    tags[b.tag].consumed -= b.consumed;
    live -= b.size;
    consumed -= b.consumed;
    sites[b.site].live -= b.size;
    sites[b.site].consumed -= b.consumed;
    if (b.fallback)
      fallback_live -= b.size;
    by_size.erase({b.size, b.id});
    blocks.erase(b.ptr);
  }
  void add(Block b) {
    if (blocks.size() >= options.block_capacity) {
      ++block_overflow;
      return;
    }
    h2_mem_arena_block_info_t info{};
    (void)h2_mem_arena_block_info(core, b.ptr, &info);
    b.consumed = info.consumed_bytes;
    b.pool = info.pool;
    b.fallback = info.fallback != 0;
    blocks.emplace(b.ptr, b);
    by_size.emplace(std::make_pair(b.size, b.id), b.ptr);
    auto &t = tags[b.tag];
    t.live += b.size;
    t.consumed += b.consumed;
    ++t.count;
    t.largest = std::max(t.largest, b.size);
    t.peak = std::max(t.peak, t.live);
    t.overhead_peak = std::max(t.overhead_peak, t.consumed - t.live);
    auto &site_stats = sites[b.site];
    site_stats.live += b.size;
    site_stats.consumed += b.consumed;
    site_stats.peak = std::max(site_stats.peak, site_stats.live);
    site_stats.overhead_peak = std::max(site_stats.overhead_peak,
                                        site_stats.consumed - site_stats.live);
    live += b.size;
    consumed += b.consumed;
    consumed_peak = std::max(consumed_peak, consumed);
    if (b.fallback)
      fallback_live += b.size;
    ++sequence;
    if (live > peak.live) {
      peak = {};
      peak.seq = sequence;
      peak.live = live;
      peak.consumed = consumed;
      peak.fallback = fallback_live;
      for (unsigned i = 0; i < tag_count; ++i) {
        peak.tags[i] = tags[i].live;
        peak.consumed_tags[i] = tags[i].consumed;
      }
      if (peaks.size() == options.peak_capacity) {
        peaks.erase(peaks.begin());
        ++peak_overflow;
      }
      peaks.push_back(peak);
      if (trace) {
        peak_blocks.clear();
        for (auto it = by_size.rbegin();
             it != by_size.rend() && peak_blocks.size() < top_count; ++it)
          peak_blocks.push_back(blocks.at(it->second));
      }
    }
  }
  static void *alloc(void *user, size_t bytes) {
    auto &view = *static_cast<View *>(user);
    auto &self = *view.self;
    const auto frames = self.capture();
    std::lock_guard<std::mutex> guard(self.mutex);
    void *ptr = h2_pal_mem_alloc(self.underlying(view.tag), bytes);
    if (ptr != nullptr) {
      if (self.trace && view.tag != self.stack_tag)
        std::memset(ptr, kPattern, bytes);
      Block b;
      b.ptr = ptr;
      b.size = bytes;
      b.tag = view.tag;
      b.id = ++self.next_id;
      b.site = self.site(frames);
      ++self.sites[b.site].requests;
      self.sites[b.site].requested += bytes;
      self.add(b);
    } else if (bytes != 0) {
      ++self.tags[view.tag].failures;
    }
    return ptr;
  }
  static void free(void *user, void *ptr) {
    if (ptr == nullptr)
      return;
    auto &view = *static_cast<View *>(user);
    auto &self = *view.self;
    // The caller owns the block exclusively during free. Scan before taking
    // the allocator lock, so unrelated tasks are never blocked by tail scans.
    Block b;
    {
      std::lock_guard<std::mutex> guard(self.mutex);
      const auto found = self.blocks.find(ptr);
      if (found == self.blocks.end()) {
        h2_pal_mem_free(self.underlying(view.tag), ptr);
        return;
      }
      b = found->second;
    }
    const size_t unused =
        self.trace && b.tag != self.stack_tag ? untouched_tail(ptr, b.size) : 0;
    std::lock_guard<std::mutex> guard(self.mutex);
    if (self.trace && b.tag != self.stack_tag) {
      self.tags[b.tag].freed_requested += b.size;
      self.tags[b.tag].freed_unused += unused;
      self.sites[b.site].freed_requested += b.size;
      self.sites[b.site].freed_unused += unused;
    }
    self.remove(b);
    ++self.sequence;
    h2_pal_mem_free(self.underlying(b.tag), ptr);
  }
  static void *realloc(void *user, void *ptr, size_t bytes) {
    if (ptr == nullptr)
      return alloc(user, bytes);
    if (bytes == 0) {
      free(user, ptr);
      return nullptr;
    }
    auto &view = *static_cast<View *>(user);
    auto &self = *view.self;
    const auto frames = self.capture();
    Block old;
    {
      std::lock_guard<std::mutex> guard(self.mutex);
      const auto found = self.blocks.find(ptr);
      if (found == self.blocks.end()) {
        void *next = h2_pal_mem_realloc(self.underlying(view.tag), ptr, bytes);
        if (next != nullptr) {
          Block b;
          b.ptr = next;
          b.size = bytes;
          b.tag = view.tag;
          b.id = ++self.next_id;
          b.site = self.site(frames);
          self.add(b);
        }
        return next;
      }
      old = found->second;
    }
    const size_t retired_unused =
        self.trace && old.tag != self.stack_tag ? untouched_tail(ptr, old.size) : 0;
    std::lock_guard<std::mutex> guard(self.mutex);
    void *next = h2_pal_mem_realloc(self.underlying(old.tag), ptr, bytes);
    if (next == nullptr) {
      ++self.tags[old.tag].failures;
      return nullptr;
    }
    if (self.trace && old.tag != self.stack_tag && bytes > old.size)
      std::memset(static_cast<unsigned char *>(next) + old.size, kPattern,
                  bytes - old.size);
    if (self.trace && old.tag != self.stack_tag) {
      self.sites[old.site].retired_requested += old.size;
      self.sites[old.site].retired_unused += retired_unused;
    }
    self.remove(old);
    Block b = old;
    b.ptr = next;
    b.size = bytes;
    // A resize is a new generation, even when TLSF kept the same address.
    b.id = ++self.next_id;
    b.site = self.site(frames);
    auto &s = self.sites[b.site];
    ++s.requests;
    s.requested += bytes;
    ++s.realloc_count;
    s.growth_bytes += bytes > old.size ? bytes - old.size : 0;
    s.moved += next != ptr;
    self.add(
        b); // Keep the original owner even when freed/resized via another view.
    if (next != ptr)
      self.realloc_overlap_upper =
          std::max(self.realloc_overlap_upper, self.consumed + old.consumed);
    return next;
  }
  inline static const h2_pal_mem_vtable_t vtable = {alloc, realloc, free};

  Sample scan(const Block &b) {
    Sample sample;
    sample.block = b;
    if (b.tag == stack_tag)
      return sample;
    std::array<unsigned char, 16384> buffer{};
    size_t used = 0;
    uint64_t used_hash = sample.hash;
    for (size_t offset = 0; offset < b.size; offset += buffer.size()) {
      const size_t bytes = std::min(buffer.size(), b.size - offset);
      {
        std::lock_guard<std::mutex> guard(mutex);
        const auto found = blocks.find(b.ptr);
        if (found == blocks.end() || found->second.id != b.id ||
            !read_memory(static_cast<unsigned char *>(b.ptr) + offset,
                         buffer.data(), bytes))
          return sample;
      }
      for (size_t i = 0; i < bytes; ++i) {
        if (b.size >= kLarge) {
          sample.hash ^= buffer[i];
          sample.hash *= 1099511628211ull;
        }
        if (buffer[i] != kPattern) {
          used = offset + i + 1;
          used_hash = sample.hash;
        }
      }
    }
    sample.hash = used_hash;
    sample.unused = b.size - used;
    sample.valid = true;
    return sample;
  }
  void write_block(std::ostream &out, const char *kind, const Block &b) {
    out << kind << " id=" << b.id << " tag=" << tag_names[b.tag]
        << " requested=" << b.size << " consumed=" << b.consumed
        << " overhead=" << b.consumed - b.size << " pool=" << b.pool
        << " fallback=" << b.fallback << " site=" << b.site;
  }
  void snapshot(const char *raw_phase) {
    if (core == nullptr)
      return;
    std::unique_lock<std::mutex> reporting(report_mutex);
    if (reporter.joinable() &&
        pending_reports.size() >= options.report_queue_capacity) {
      ++report_backpressure;
      report_done.wait(reporting, [this] {
        return pending_reports.size() < options.report_queue_capacity;
      });
    }
    const auto started = Clock::now();
    Snapshot value;
    value.report_backpressure = report_backpressure;
    value.phase = token(raw_phase);
    value.seq = ++snapshot_sequence;
    auto &phase = value.phase;
    auto &copy = value.copy;
    auto &top = value.top;
    auto &history = value.history;
    auto &new_sites = value.new_sites;
    auto &first_site = value.first_site;
    auto &tag_copy = value.tag_copy;
    auto &pools = value.pools;
    auto &stats = value.stats;
    auto &peak_copy = value.peak_copy;
    auto &seq = value.allocation_seq;
    auto &total = value.total;
    auto &actual = value.actual;
    {
      std::lock_guard<std::mutex> guard(mutex);
      for (const auto &entry : blocks)
        copy.push_back(entry.second);
      tag_copy = tags;
      top = peak_blocks;
      peak_copy = peak;
      history.swap(peaks);
      first_site = reported_sites;
      new_sites.assign(sites.begin() + first_site, sites.end());
      reported_sites = sites.size();
      seq = sequence;
      total = live;
      actual = consumed;
      value.block_overflow = block_overflow;
      value.site_overflow = site_overflow;
      value.peak_overflow = peak_overflow;
      (void)h2_mem_arena_inspect(core, pools);
      (void)h2_mem_arena_stats(core, &stats);
      if (census != nullptr) {
        (void)h2_mem_arena_census_snapshot(
            census,
            [](void *user, const h2_mem_arena_census_snapshot_t *current) {
              auto &snapshot = *static_cast<Snapshot *>(user);
              for (size_t i = 0; i < current->tag_count && i < kMaxTags; ++i) {
                snapshot.census_live[i] = current->tags[i].counts.live_bytes;
                snapshot.census_peak[i] = current->tags[i].counts.peak_bytes;
              }
              snapshot.census_unattributed_live =
                  current->unattributed_overflow.live_bytes;
              snapshot.census_block_overflow = current->block_overflow;
              snapshot.census_site_overflow = current->site_overflow;
            },
            &value);
      }
      if (phase.compare(0, 9, "case_end:") == 0) {
        if (case_ends == 0) {
          for (const auto &b : copy)
            residents.emplace(b.id, b);
        } else {
          for (auto it = residents.begin(); it != residents.end();) {
            const auto found = blocks.find(it->second.ptr);
            if (found == blocks.end() || found->second.id != it->first)
              it = residents.erase(it);
            else
              ++it;
          }
        }
        for (auto &t : tags) {
          if (case_ends == 0)
            t.end_first = t.end_min = t.end_max = t.live;
          t.end_last = t.live;
          t.end_min = std::min(t.end_min, t.live);
          t.end_max = std::max(t.end_max, t.live);
        }
        ++case_ends;
      }
    }
    value.captured_at = Clock::now();
    value.capture_us = std::chrono::duration_cast<std::chrono::microseconds>(
                           value.captured_at - started)
                           .count();
    if (reporter.joinable()) {
      pending_reports.push_back(std::move(value));
      report_ready.notify_one();
    } else {
      render(value);
    }
  }
  void flush() {
    std::unique_lock<std::mutex> guard(report_mutex);
    report_done.wait(
        guard, [this] { return pending_reports.empty() && !reporter_active; });
  }
  void run_reports() {
    for (;;) {
      Snapshot value;
      {
        std::unique_lock<std::mutex> guard(report_mutex);
        report_ready.wait(guard, [this] {
          return reporter_stop || !pending_reports.empty();
        });
        if (pending_reports.empty())
          return;
        value = std::move(pending_reports.front());
        pending_reports.pop_front();
        reporter_active = true;
      }
      render(value);
      {
        std::lock_guard<std::mutex> guard(report_mutex);
        reporter_active = false;
      }
      report_done.notify_all();
    }
  }
  void render(const Snapshot &value) {
    const auto started = Clock::now();
    const auto &phase = value.phase;
    const auto &copy = value.copy;
    const auto &top = value.top;
    const auto &history = value.history;
    const auto &new_sites = value.new_sites;
    const auto first_site = value.first_site;
    const auto &tag_copy = value.tag_copy;
    const auto &pools = value.pools;
    const auto &stats = value.stats;
    const auto &peak_copy = value.peak_copy;
    const auto seq = value.seq;
    const auto total = value.total;
    const auto actual = value.actual;
    for (size_t i = 0; i < new_sites.size(); ++i) {
      output << "SITE id=" << first_site + i;
      for (size_t j = 0; j < new_sites[i].frames.size(); ++j)
        output << " frame" << j << '=' << new_sites[i].frames[j];
      output << '\n';
    }
    for (const auto &p : history) {
      output << "PEAK seq=" << p.seq << " requested=" << p.live
             << " consumed=" << p.consumed << " fallback=" << p.fallback;
      for (unsigned i = 0; i < tag_count; ++i)
        output << ' ' << tag_names[i] << '=' << p.tags[i] << ' ' << tag_names[i]
               << "_consumed=" << p.consumed_tags[i];
      output << '\n';
    }
    size_t census_total = value.census_unattributed_live;
    for (size_t i = 0; i < tag_count; ++i)
      census_total += value.census_live[i];
    if (census == nullptr)
      census_total = total;
    output << "SNAPSHOT phase=" << phase << " seq=" << seq
           << " allocation_seq=" << value.allocation_seq << " live=" << total
           << " census_live=" << census_total
           << " consumed=" << actual << " peak=" << peak_copy.live
           << " small_free=" << pools[0].free_bytes
           << " small_largest=" << pools[0].largest_free_block
           << " large_free=" << pools[1].free_bytes
           << " large_largest=" << pools[1].largest_free_block
           << " fallback=" << stats.fallback_live_bytes << '\n';
    Counts unused{}, scanned{};
    std::map<unsigned, size_t> site_unused;
    std::map<std::pair<size_t, uint64_t>, Sample> hashes;
    size_t skipped = 0, duplicates = 0;
    if (trace) {
      for (const auto &b : copy) {
        if (b.tag == stack_tag)
          continue;
        const auto sample = scan(b);
        if (!sample.valid) {
          ++skipped;
          continue;
        }
        unused[b.tag] += sample.unused;
        site_unused[b.site] += sample.unused;
        scanned[b.tag] += b.size;
        // Report all scanned live blocks so unused bytes can be grouped by
        // call site without conflating repeated snapshots with allocations.
        write_block(output, "BLOCK", b);
        output << " seq=" << seq << " unused=" << sample.unused
               << " used_estimate=" << b.size - sample.unused << '\n';
        if (b.size - sample.unused >= kLarge) {
          const auto key = std::make_pair(b.size - sample.unused, sample.hash);
          const auto found = hashes.find(key);
          bool co_live = false;
          if (found != hashes.end()) {
            std::lock_guard<std::mutex> guard(mutex);
            const auto first = blocks.find(found->second.block.ptr);
            const auto second = blocks.find(b.ptr);
            co_live = first != blocks.end() &&
                      first->second.id == found->second.block.id &&
                      second != blocks.end() && second->second.id == b.id;
          }
          if (co_live) {
            // Hash matches are candidates, not proof of equivalent objects.
            output << "DUPLICATE seq=" << seq << " id=" << b.id
                   << " other=" << found->second.block.id
                   << " bytes=" << b.size - sample.unused
                   << " capacity=" << b.size
                   << " other_capacity=" << found->second.block.size
                   << " hash=" << sample.hash << " kind=hash_candidate\n";
            duplicates += b.size - sample.unused;
            const auto ids = std::minmax(b.id, found->second.block.id);
            const auto key = std::make_pair(ids.first, ids.second);
            const auto existing = duplicate_candidates.find(key);
            if (existing != duplicate_candidates.end() ||
                duplicate_candidates.size() < options.duplicate_capacity) {
              auto &candidate = duplicate_candidates[key];
              if (b.size - sample.unused > candidate.bytes)
                candidate = {ids.first, ids.second, seq, b.size - sample.unused};
            } else {
              ++duplicate_overflow;
            }
          } else
            hashes.insert_or_assign(key, sample);
        }
      }
      for (const auto &b : top) {
        write_block(output, "PEAK_BLOCK", b);
        output << " peak_seq=" << peak_copy.seq << " snapshot_seq=" << seq
               << '\n';
      }
    }
    for (unsigned i = 0; i < tag_count; ++i) {
      const auto &t = tag_copy[i];
      output << "TAG seq=" << seq << " tag=" << tag_names[i] << " live=" << t.live
             << " peak=" << t.peak << " allocations=" << t.count
             << " census_live=" << value.census_live[i]
             << " census_peak=" << value.census_peak[i]
             << " failures=" << t.failures << " largest=" << t.largest
             << " consumed=" << t.consumed << " scanned=" << scanned[i]
             << " unused=" << unused[i] << " scan_exempt=" << (i == stack_tag)
             << '\n';
    }
    {
      std::lock_guard<std::mutex> guard(mutex);
      for (unsigned i = 0; i < tag_count; ++i)
        tags[i].unused_max = std::max(tags[i].unused_max, unused[i]);
      for (const auto &entry : site_unused)
        sites[entry.first].unused_snapshot_max =
            std::max(sites[entry.first].unused_snapshot_max, entry.second);
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                             Clock::now() - started)
                             .count();
    output << "SCAN seq=" << seq << " skipped=" << skipped
           << " duplicate_candidate_bytes=" << duplicates
           << " elapsed_us=" << elapsed << " capture_us=" << value.capture_us
           << " queue_us="
           << std::chrono::duration_cast<std::chrono::microseconds>(
                  started - value.captured_at)
                  .count()
           << '\n';
    output << "OVERFLOW seq=" << seq << " blocks=" << value.block_overflow
           << " sites=" << value.site_overflow
           << " peaks=" << value.peak_overflow
           << " duplicates=" << duplicate_overflow
           << " report_backpressure=" << value.report_backpressure
           << " census_blocks=" << value.census_block_overflow
           << " census_sites=" << value.census_site_overflow
           << " census_unattributed_live="
           << value.census_unattributed_live << '\n';
    output.flush();
    std::fprintf(stderr,
                 "%s phase=%s live=%zu census_live=%zu peak=%zu small_live=%zu "
                 "large_live=%zu fallback=%zu small_free=%zu small_largest=%zu "
                 "large_free=%zu large_largest=%zu scan_us=%lld\n",
                 console_prefix.c_str(), phase.c_str(), total, census_total,
                 peak_copy.live,
                 stats.small.live_bytes,
                 stats.large.live_bytes, stats.fallback_live_bytes,
                 pools[0].free_bytes, pools[0].largest_free_block,
                 pools[1].free_bytes, pools[1].largest_free_block,
                 static_cast<long long>(elapsed));
  }
  void summarize() {
    Counts retained{};
    for (const auto &entry : residents)
      retained[entry.second.tag] += entry.second.size;
    std::vector<unsigned> order(tag_count);
    for (unsigned i = 0; i < tag_count; ++i)
      order[i] = i;
    std::sort(order.begin(), order.end(), [this](unsigned a, unsigned b) {
      return peak.tags[a] > peak.tags[b];
    });
    for (unsigned i : order) {
      const auto &t = tags[i];
      output << "SUMMARY_TAG tag=" << tag_names[i]
             << " at_total_peak=" << peak.tags[i]
             << " consumed_at_total_peak=" << peak.consumed_tags[i]
             << " peak_share_pct="
             << (peak.live != 0 ? 100.0 * peak.tags[i] / peak.live : 0)
             << " own_peak=" << t.peak << " overhead_peak=" << t.overhead_peak
             << " live_at_exit=" << t.live << " allocations=" << t.count
             << " failures=" << t.failures << " largest=" << t.largest
             << " case_ends=" << case_ends
             << " resident_same_blocks=" << retained[i]
             << " end_min=" << t.end_min << " end_max=" << t.end_max
             << " end_growth="
             << static_cast<int64_t>(t.end_last) -
                    static_cast<int64_t>(t.end_first)
             << " stable_end_bytes="
             << (case_ends > 1 && t.end_min == t.end_max ? t.end_min : 0)
             << " unused_snapshot_max=" << t.unused_max
             << " freed_requested=" << t.freed_requested
             << " freed_unused=" << t.freed_unused << '\n';
    }
    for (size_t i = 0; i < sites.size(); ++i) {
      const auto &s = sites[i];
      output << "SITE id=" << i << " requests=" << s.requests
             << " requested=" << s.requested
             << " freed_requested=" << s.freed_requested
             << " freed_unused=" << s.freed_unused << " live=" << s.live
             << " own_peak=" << s.peak
             << " retired_requested=" << s.retired_requested
             << " retired_unused=" << s.retired_unused
             << " reallocs=" << s.realloc_count
             << " growth_bytes=" << s.growth_bytes << " moved=" << s.moved;
      for (size_t j = 0; j < s.frames.size(); ++j)
        output << " frame" << j << '=' << s.frames[j];
      output << '\n';
    }
    // Separate rank records make the raw end report useful without a parser.
    const auto rank = [&](const char *category, const Counts &values) {
      std::sort(order.begin(), order.end(),
                [&](unsigned a, unsigned b) { return values[a] > values[b]; });
      for (unsigned i : order)
        output << "RANK category=" << category << " tag=" << tag_names[i]
               << " bytes=" << values[i] << '\n';
    };
    rank("resident", retained);
    Counts values{};
    for (unsigned i = 0; i < tag_count; ++i)
      values[i] = tags[i].end_last > tags[i].end_first
                      ? tags[i].end_last - tags[i].end_first
                      : 0;
    rank("growth", values);
    for (unsigned i = 0; i < tag_count; ++i)
      values[i] = tags[i].unused_max;
    rank("unused", values);
    for (unsigned i = 0; i < tag_count; ++i)
      values[i] = tags[i].overhead_peak;
    rank("overhead", values);
    std::vector<unsigned> site_order;
    for (unsigned i = 0; i < sites.size(); ++i)
      site_order.push_back(i);
    std::sort(site_order.begin(), site_order.end(),
              [this](unsigned a, unsigned b) {
                return sites[a].peak > sites[b].peak;
              });
    for (unsigned i : site_order)
      output << "RANK_SITE site=" << i << " own_peak=" << sites[i].peak << '\n';
    std::vector<Duplicate> duplicate_order;
    for (const auto &entry : duplicate_candidates)
      duplicate_order.push_back(entry.second);
    std::sort(duplicate_order.begin(), duplicate_order.end(),
              [](const Duplicate &a, const Duplicate &b) {
                return a.bytes > b.bytes;
              });
    for (const auto &d : duplicate_order)
      output << "RANK_DUPLICATE id=" << d.first << " other=" << d.second
             << " bytes=" << d.bytes << " snapshot=" << d.snapshot << '\n';
    output << "END live=" << live << " peak=" << peak.live
           << " consumed_peak=" << consumed_peak
           << " moved_realloc_overlap_upper=" << realloc_overlap_upper
           << " blocks=" << blocks.size() << '\n';
    output.flush();
  }
};

DesktopArenaDiagnostics::DesktopArenaDiagnostics(
    const DesktopDiagnosticsConfig &config, const h2_pal_mem_api_t *fallback)
    : impl_(std::make_unique<Impl>(config, fallback)) {}
DesktopArenaDiagnostics::~DesktopArenaDiagnostics() = default;
const h2_pal_mem_api_t *DesktopArenaDiagnostics::mem() const { return tagged(nullptr); }
const h2_pal_mem_api_t *DesktopArenaDiagnostics::tagged(const char *owner) const {
  if (impl_->core == nullptr)
    return nullptr;
  if (owner != nullptr)
    for (size_t i = 0; i < impl_->tag_count; ++i)
      if (std::strcmp(owner, impl_->tag_names[i]) == 0)
        return &impl_->views[i].api;
  return &impl_->views[0].api;
}
DesktopArenaDiagnostics::Stats DesktopArenaDiagnostics::stats() {
  Stats s{};
  (void)h2_mem_arena_stats(impl_->core, &s);
  return s;
}
void DesktopArenaDiagnostics::report(const char *phase) { impl_->snapshot(phase); }
void DesktopArenaDiagnostics::flush() { impl_->flush(); }
void DesktopArenaDiagnostics::poll() {
  const auto now = Clock::now();
  if (!impl_->trace || now - impl_->polled_at < std::chrono::seconds(1))
    return;
  impl_->polled_at = now;
  bool changed;
  {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    changed = impl_->peak.live >= impl_->polled_peak + 32u * 1024u;
    if (changed)
      impl_->polled_peak = impl_->peak.live;
  }
  if (changed)
    report("peak_sample");
}
} // namespace h2::mem_arena
