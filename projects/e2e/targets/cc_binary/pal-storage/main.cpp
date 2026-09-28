#include "h2_desktop_platform.h"
#include "h2_pal_storage_e2e.h"
#include "host_config.h"
#include "h2_sqlite.h"
#if defined(__APPLE__)
#include "h2_darwin_platform.h"
#define host_fs_t h2_darwin_host_fs_t
#define host_fs_create h2_darwin_host_fs_create
#define host_fs_api h2_darwin_host_fs_api
#define host_fs_destroy h2_darwin_host_fs_destroy
#else
#include "h2_linux_platform.h"
#define host_fs_t h2_linux_host_fs_t
#define host_fs_create h2_linux_host_fs_create
#define host_fs_api h2_linux_host_fs_api
#define host_fs_destroy h2_linux_host_fs_destroy
#endif
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace {
union Header {
  std::max_align_t alignment;
  std::size_t size;
};
std::atomic<size_t> allocations{0}, bytes{0};
void *allocate(void *, size_t size) {
  if (size > SIZE_MAX - sizeof(Header))
    return nullptr;
  auto *p = static_cast<Header *>(std::malloc(sizeof(Header) + size));
  if (!p)
    return nullptr;
  p->size = size;
  ++allocations;
  bytes += size;
  return p + 1;
}
void release(void *, void *pointer) {
  if (!pointer)
    return;
  auto *p = static_cast<Header *>(pointer) - 1;
  --allocations;
  bytes -= p->size;
  std::free(p);
}
void *resize(void *user, void *pointer, size_t size) {
  if (!pointer)
    return allocate(user, size);
  if (!size) {
    release(user, pointer);
    return nullptr;
  }
  auto *old = static_cast<Header *>(pointer) - 1;
  void *next = allocate(user, size);
  if (!next)
    return nullptr;
  std::memcpy(next, pointer, old->size < size ? old->size : size);
  release(user, pointer);
  return next;
}
const h2_pal_mem_vtable_t memory_methods = {allocate, resize, release};
const h2_pal_mem_api_t memory = {nullptr, &memory_methods};
struct Reporter {
  unsigned phase;
  uint32_t nonce;
};
void report(void *user, const char *id, h2_pal_storage_status_t status,
            h2_pal_result_t rc) {
  auto *reporter = static_cast<Reporter *>(user);
  static const char *names[] = {"NOT_RUN", "PASS", "FAIL", "BLOCKED"};
  std::printf(
      "H2_STORAGE_CASE "
      "{\"id\":\"%s\",\"status\":\"%s\",\"rc\":%d,\"phase\":%u,\"nonce\":%u}\n",
      id, names[status], rc, reporter->phase, reporter->nonce);
  std::fflush(stdout);
}
} // namespace
int main(int argc, char **argv) {
  if (argc != 4)
    return 2;
  unsigned phase = static_cast<unsigned>(std::strtoul(argv[1], nullptr, 10));
  uint32_t nonce = static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 10));
  std::filesystem::path root = argv[2];
  const std::string fs_path = (root / "files").string(),
                    db_path = (root / "preferences.sqlite").string();
  const char *sources[] = {fs_path.c_str()}, *targets[] = {"/storage"};
  host_fs_t *fs = nullptr;
  h2_sqlite_t *pref = nullptr;
  h2_runtime_t *runtime = nullptr;
  if (host_fs_create(sources, targets, 1, &fs) != H2_PAL_OK)
    return 3;
  const h2_sqlite_config_t database = {db_path.c_str()};
  if (h2_sqlite_create(&database, &pref) != H2_PAL_OK) {
    host_fs_destroy(fs);
    return 4;
  }
  auto config = h2_storage_host_config(&memory);
  config.fs = host_fs_api(fs);
  config.pref = h2_sqlite_pref_api(pref);
  if (h2_runtime_init(&config, &runtime) != H2_PAL_OK) {
    h2_sqlite_destroy(pref);
    host_fs_destroy(fs);
    return 5;
  }
  const size_t before_allocations = allocations.load(),
               before_bytes = bytes.load();
  Reporter reporter{phase, nonce};
  h2_pal_storage_config_t tests{};
  tests.root = "/storage/run";
  tests.namespace_a = "h2storea";
  tests.namespace_b = "h2storeb";
  tests.nonce = nonce;
  tests.phase = static_cast<h2_pal_storage_phase_t>(phase);
  tests.case_result = report;
  tests.user = &reporter;
  h2_pal_storage_result_t result{};
  const int rc = h2_pal_storage_e2e_run(runtime, &tests, &result);
  bool balanced = allocations == before_allocations && bytes == before_bytes;
  std::printf(
      "H2_STORAGE_PHASE "
      "{\"phase\":%u,\"nonce\":%u,\"pid\":%ld,\"passed\":%zu,\"failed\":%zu,"
      "\"blocked\":%zu,\"cleanup\":%d,\"balanced\":%s}\n",
      phase, nonce, static_cast<long>(getpid()), result.passed, result.failed,
      result.blocked, result.cleanup_result, balanced ? "true" : "false");
  if (result.retained_cleanup)
    return 6;
  h2_runtime_deinit(runtime);
  h2_sqlite_destroy(pref);
  host_fs_destroy(fs);
  return rc == 0 && balanced && allocations == 0 && bytes == 0 ? 0 : 1;
}
