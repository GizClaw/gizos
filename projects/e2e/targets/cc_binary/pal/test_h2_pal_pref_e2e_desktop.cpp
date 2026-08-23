#include "h2_pal_e2e.h"
#include "h2_sqlite.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <unistd.h>

namespace {

void *desktop_alloc(void *, std::size_t size) { return std::malloc(size); }
void *desktop_realloc(void *, void *ptr, std::size_t size) {
  return std::realloc(ptr, size);
}
void desktop_free(void *, void *ptr) { std::free(ptr); }

const h2_pal_mem_vtable_t kMemoryVtable = {
    desktop_alloc,
    desktop_realloc,
    desktop_free,
};
const h2_pal_mem_api_t kMemory = {nullptr, &kMemoryVtable};

}  // namespace

int main() {
  const char *test_tmpdir = std::getenv("TEST_TMPDIR");
  std::filesystem::path root =
      std::filesystem::path(test_tmpdir != nullptr ? test_tmpdir : "/tmp") /
      ("h2-pal-pref-e2e-" + std::to_string(static_cast<long>(getpid())));
  std::error_code error;
  if (!std::filesystem::create_directory(root, error)) {
    std::fprintf(stderr, "H2_PAL_PREF_E2E_DESKTOP_FAIL stage=prepare\n");
    return 1;
  }
  h2_pal_e2e_config_t config = {};
  config.suite_mask = H2_PAL_E2E_SUITE_PREF;
  int status = 0;
  for (unsigned pass = 0u; pass < 4u; ++pass) {
    h2_sqlite_t *preference = nullptr;
    h2_runtime_t runtime = {};
    const std::string database = (root / "prefs.sqlite").string();
    const h2_sqlite_config_t sqlite_config = {database.c_str()};
    if (h2_sqlite_create(&sqlite_config, &preference) != H2_PAL_OK) {
      status = 1;
      break;
    }
    runtime.mem = &kMemory;
    runtime.pref = h2_sqlite_pref_api(preference);
    h2_pal_e2e_result_t result = {};
    const int rc = h2_pal_e2e_run(&runtime, &config, &result);
    std::fprintf(stderr,
                 "H2_PAL_PREF_E2E_DESKTOP phase=%d action=%d rc=%d "
                 "selected=%zu passed=%zu failed=%zu status=%s\n",
                 result.pref_phase, result.action, rc, result.selected,
                 result.passed, result.failed,
                 rc == H2_PAL_OK ? "PASS" : "FAIL");
    h2_sqlite_destroy(preference);
    if (rc != H2_PAL_OK ||
        (pass < 2u && result.action != H2_PAL_E2E_ACTION_REBOOT) ||
        (pass >= 2u && result.action != H2_PAL_E2E_ACTION_NONE)) {
      status = 1;
      break;
    }
  }
  std::filesystem::remove_all(root, error);
  if (status == 0) {
    std::fprintf(stderr, "H2_PAL_PREF_E2E_DESKTOP_READY status=PASS\n");
  }
  return status;
}
