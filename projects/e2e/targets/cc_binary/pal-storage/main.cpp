#include "h2_desktop_application_host.h"
#include "h2_pal_storage_e2e.h"
#include "device_runner.h"
#include "host_config.h"
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
static Reporter reporter{};
static bool device_control;
static const char *version;
static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *runtime, void *) {
  const size_t before_allocations = allocations.load(),
               before_bytes = bytes.load();
  h2_pal_storage_config_t tests{};
  tests.root = "/storage/run";
  tests.namespace_a = "h2storea";
  tests.namespace_b = "h2storeb";
  tests.nonce = reporter.nonce;
  tests.phase = static_cast<h2_pal_storage_phase_t>(reporter.phase);
  tests.case_result = report;
  tests.user = &reporter;
  h2_pal_storage_result_t result{};
  const int rc = device_control
                     ? h2_storage_device_run(runtime, tests.root, version)
                     : h2_pal_storage_e2e_run(runtime, &tests, &result);
  if (device_control)
    h2_storage_device_replay(runtime);
  bool balanced = allocations == before_allocations && bytes == before_bytes;
  if (device_control) {
    std::printf(
        "H2_STORAGE_DEVICE_EXIT {\"rc\":%d,\"pid\":%ld,\"balanced\":%s}\n", rc,
        static_cast<long>(getpid()), balanced ? "true" : "false");
  } else {
    std::printf("H2_STORAGE_PHASE "
                "{\"contract\":%u,\"phase\":%u,\"nonce\":%u,\"pid\":%ld,"
                "\"passed\":%zu,\"failed\":%zu,"
                "\"blocked\":%zu,\"cleanup\":%d,\"balanced\":%s}\n",
                H2_PAL_STORAGE_CONTRACT_VERSION, reporter.phase, reporter.nonce,
                static_cast<long>(getpid()), result.passed, result.failed,
                result.blocked, result.cleanup_result,
                balanced ? "true" : "false");
  }
  if (result.retained_cleanup || (device_control && !balanced))
    h2_application_host_retain(host, result.retained_cleanup, H2_PAL_ERR_BUSY);
  return rc == 0 && balanced ? H2_PAL_OK : H2_PAL_ERR_IO;

}
int main(int argc, char **argv) {
  if (argc != 4 && argc != 5)
    return 2;
  unsigned phase = static_cast<unsigned>(std::strtoul(argv[1], nullptr, 10));
  uint32_t nonce = static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 10));
  std::filesystem::path root = argv[2];
  const std::string fs_path = (root / "files").string(),
                    db_path = (root / "preferences.sqlite").string();
  const h2_desktop_filesystem_mount_t mount = {fs_path.c_str(), "/storage"};
  h2_desktop_application_host_config_t platform{};
  platform.runtime = h2_storage_host_config(&memory);
  platform.mounts = &mount;
  platform.mount_count = 1u;
  platform.services = H2_DESKTOP_HOST_PREFERENCE;
  platform.preference_path = db_path.c_str();
  reporter = {phase, nonce};
  device_control = argc == 5;
  version = device_control ? argv[4] : nullptr;
  h2_application_host_config_t app{};
  app.entry = run;
  h2_application_host_result_t lifecycle{};
  int rc = h2_desktop_application_host_run(&platform, &app, &lifecycle);
  if (lifecycle.retained) return 6;
  return rc == 0 && lifecycle.cleanup == H2_PAL_OK && allocations == 0 && bytes == 0 ? 0 : 1;
}
