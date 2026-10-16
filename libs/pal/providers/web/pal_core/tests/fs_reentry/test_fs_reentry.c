#include "h2_web_fs.h"
#include "h2_web_main_thread.h"
#include "h2_web_platform.h"
#include <assert.h>
#include <emscripten.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static h2_web_platform_t *platform;
static _Atomic(h2_web_fs_t *) current_fs;
static _Atomic unsigned callbacks, opening_callbacks;
static _Atomic int opening;

/* Runs on the UI inside real proxied POSIX syscalls. A state callback must
 * enter the same provider guard without blocking the syscall's Worker. */
EMSCRIPTEN_KEEPALIVE int h2_test_fs_reentry(void) {
  H2_WEB_STATE_GUARD();
  h2_web_fs_t *fs = atomic_load(&current_fs);
  if (fs != NULL) {
    h2_web_fs_status_t status;
    if (h2_web_fs_get_status(fs, &status) != H2_PAL_OK ||
        h2_web_fs_close(fs) != H2_PAL_ERR_BUSY)
      return 1;
    if (atomic_load(&opening))
      atomic_fetch_add(&opening_callbacks, 1u);
  }
  if (h2_web_platform_destroy(platform) != H2_PAL_ERR_BUSY)
    return 2;
  atomic_fetch_add(&callbacks, 1u);
  return 0;
}

/* clang-format off */
EM_JS_DEPS(fs_reentry, "$FS");
EM_JS(void, hooks,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ['i32'], 'i32', enable => {
    if (!enable) {
      for (const [name, original] of Module.h2FsReentryMethods) FS[name] = original;
      delete Module.h2FsReentryMethods;
      return 0;
    }
    Module.h2FsReentryMethods = new Map();
    for (const name of ['mkdir','open','read','write','close','stat','lstat','llseek','rename','unlink','rmdir','readdir']) {
      const original = FS[name];
      if (typeof original !== 'function') continue;
      Module.h2FsReentryMethods.set(name, original);
      FS[name] = function(...args) {
        const path = typeof args[0] === 'string' ? args[0] : args[0]?.path || "";
        if ((path === '/fs-reentry' || path.startsWith('/fs-reentry/')) &&
            Module._h2_test_fs_reentry() !== 0) throw new Error('FS callback lost an active borrow');
        return original.apply(this, args);
      };
    }
    return 0;
  });
});
/* clang-format on */

int main(void) {
  platform = h2_web_platform_create(&(h2_web_platform_config_t){64, 64});
  assert(platform != NULL);
  int enabled = 1;
  assert(h2_web_main_call(hooks, (const void *[]){&enabled}).i32 == 0);
  h2_web_fs_t *fs = NULL;
  assert(h2_web_fs_open(platform,
      &(h2_web_fs_config_t){.persistent_root = "/fs-reentry"}, &fs) == H2_PAL_OK);
  atomic_store(&current_fs, fs);
  const h2_pal_fs_api_t *api = h2_web_fs_api(fs);
  assert(h2_pal_fs_mkdir(api, "/fs-reentry/probe") == H2_PAL_OK);
  atomic_store(&opening, 1);
  h2_pal_fs_file_t *file = NULL;
  assert(h2_pal_fs_open(api, "/fs-reentry/probe/data",
      H2_PAL_FS_OPEN_WRITE_TRUNCATE, &file) == H2_PAL_OK);
  atomic_store(&opening, 0);
  assert(atomic_load(&opening_callbacks) != 0u);
  static const char value[] = "actual POSIX proxy reentry";
  size_t count = 0;
  assert(h2_pal_fs_write(api, file, value, sizeof(value), &count) == H2_PAL_OK);
  assert(count == sizeof(value));
  assert(h2_pal_fs_sync(api, file) == H2_PAL_OK);
  assert(h2_pal_fs_close(api, file) == H2_PAL_OK);
  assert(h2_pal_fs_open(api, "/fs-reentry/probe/data", H2_PAL_FS_OPEN_READ,
      &file) == H2_PAL_OK);
  char output[sizeof(value)] = {0};
  assert(h2_pal_fs_read(api, file, output, sizeof(output), &count) == H2_PAL_OK);
  assert(count == sizeof(value) && memcmp(value, output, sizeof(value)) == 0);
  assert(h2_pal_fs_seek(api, file, 0u) == H2_PAL_OK);
  assert(h2_pal_fs_close(api, file) == H2_PAL_OK);
  assert(h2_pal_fs_rename(api, "/fs-reentry/probe/data",
      "/fs-reentry/probe/renamed") == H2_PAL_OK);
  h2_pal_fs_stat_t stat;
  assert(h2_pal_fs_stat(api, "/fs-reentry/probe/renamed", &stat) == H2_PAL_OK);
  assert(h2_pal_fs_remove(api, "/fs-reentry/probe/renamed") == H2_PAL_OK);
  assert(h2_web_fs_clear(fs) == H2_PAL_OK);
  assert(atomic_load(&callbacks) > 8u);
  puts("FS guard_reentry=PASS");
  puts("FS borrow_retained_during_io=PASS");
  assert(h2_web_fs_close(fs) == H2_PAL_OK);
  atomic_store(&current_fs, NULL);
  enabled = 0;
  assert(h2_web_main_call(hooks, (const void *[]){&enabled}).i32 == 0);
  assert(h2_web_platform_destroy(platform) == H2_PAL_OK);
  puts("FS teardown=PASS");
  return 0;
}
