#include "app_config.h"

#include "os/os_api.h"

#include "h2_pal_e2e.h"
#include "h2_pal_e2e_task_names.h"
#include "h2_jieli_ac791n_devkit.h"
#include "h2_jieli_ac791n_devkit_partitions.h"
#include "h2_jieli_wl82_platform_core.h"
#include "jieli_h2loader_app_support.h"
#include "jieli_app_iostreamikcp.h"
#include "device/device.h"
#include "fs/fs.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "system/task.h"
#include "system/timer.h"

static h2_loader_app_client_t loader_client;
static h2_pal_fs_api_t loader_fs;
static uint32_t next_partition = H2_JIELI_PARTITION_APP;

/* Diagnostic progress must not depend on the SDK's low-priority printf drain.
 * The board sink still serializes complete writes with the Loader transport. */
static void trace(const char *format, ...) {
  char line[192];
  va_list args;
  va_start(args, format);
  int length = vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  if (length > 0) {
    size_t count = (size_t)length < sizeof(line) ? (size_t)length : sizeof(line) - 1u;
    (void)h2_jieli_ac791n_devkit_console_write(line, count, 100u);
  }
}

void h2_jieli_sd_fs_trace_mkdir(
    const char *path, int native_result, int lookup_result) {
  trace("H2_PAL_FS mkdir path=%s native=%d lookup=%d\r\n",
        path, native_result, lookup_result);
}

static const h2_pal_fs_api_t *base_fs;

static void fs_probe_rc(const char *name, const char *step, int rc) {
  trace("H2_PAL_FS_PROBE name=%s step=%s rc=%d\r\n", name, step, rc);
}

static void fs_probe_stat(const char *name, const char *step, const char *path) {
  h2_pal_fs_stat_t st = {0};
  int rc = h2_pal_fs_stat(base_fs, path, &st);
  trace("H2_PAL_FS_PROBE name=%s step=%s rc=%d is_dir=%d size=%llu\r\n",
        name, step, rc, st.is_dir, (unsigned long long)st.size);
}

static int fs_probe_create(const char *path) {
  h2_pal_fs_file_t *file = NULL;
  int rc = h2_pal_fs_open(base_fs, path, H2_PAL_FS_OPEN_WRITE_TRUNCATE, &file);
  if (file != NULL) {
    int close_rc = h2_pal_fs_close(base_fs, file);
    if (rc == H2_PAL_OK) rc = close_rc;
  }
  return rc;
}

static void fs_probe_cleanup(const char *name) {
  fs_probe_rc(name, "clear", h2_pal_fs_clear(base_fs, "/data/h2-probe"));
}

static void fs_probe_name(const char *name, const char *path, int transfer) {
  h2_pal_fs_file_t *file = NULL;
  size_t count = 0u;
  char data[4] = {0};
  int rc = h2_pal_fs_open(base_fs, path, H2_PAL_FS_OPEN_WRITE_TRUNCATE, &file);
  fs_probe_rc(name, "open_write", rc);
  if (transfer) {
    fs_probe_rc(name, "write", h2_pal_fs_write(base_fs, file, "test", 4u, &count));
  }
  fs_probe_rc(name, "close_write", h2_pal_fs_close(base_fs, file));
  file = NULL;
  fs_probe_stat(name, "stat", path);
  if (transfer) {
    fs_probe_rc(name, "open_read", h2_pal_fs_open(
        base_fs, path, H2_PAL_FS_OPEN_READ, &file));
    rc = h2_pal_fs_read(base_fs, file, data, sizeof(data), &count);
    if (rc == H2_PAL_OK && (count != 4u || memcmp(data, "test", 4u) != 0))
      rc = H2_PAL_ERR_IO;
    fs_probe_rc(name, "read", rc);
    fs_probe_rc(name, "close_read", h2_pal_fs_close(base_fs, file));
  }
  fs_probe_rc(name, "remove", h2_pal_fs_remove(base_fs, path));
}

static void fs_probe_native(const char *name, const char *step, int native) {
  trace("H2_PAL_FS_PROBE name=%s step=%s rc=%d native=%d\r\n",
        name, step, native == 0 ? H2_PAL_OK : H2_PAL_ERR_IO, native);
}

static void fs_probe_native_open(const char *name, const char *step, FILE *file) {
  /* Record NULL/non-NULL without truncating a pointer to an integer. */
  trace("H2_PAL_FS_PROBE name=%s step=%s rc=%d native=%d\r\n",
        name, step, file == NULL ? H2_PAL_ERR_IO : H2_PAL_OK, file != NULL);
}

static void run_fs_probes(void) {
  const char *a = "/data/h2-probe/a";
  const char *b = "/data/h2-probe/b";
  const char *d = "/data/h2-probe/d";
  const char *f = "/data/h2-probe/f";
  const char *name = "setup";
  fs_probe_rc(name, "mkdir", h2_pal_fs_mkdir(base_fs, "/data/h2-probe"));
  fs_probe_cleanup(name);

  name = "rename_replace";
  fs_probe_rc(name, "create_a", fs_probe_create(a));
  fs_probe_rc(name, "create_b", fs_probe_create(b));
  fs_probe_rc(name, "rename", h2_pal_fs_rename(base_fs, a, b));
  fs_probe_stat(name, "stat_b", b);
  fs_probe_stat(name, "stat_a", a);
  fs_probe_cleanup(name);

  name = "rename_same";
  fs_probe_rc(name, "create_a", fs_probe_create(a));
  fs_probe_rc(name, "rename", h2_pal_fs_rename(base_fs, a, a));
  fs_probe_cleanup(name);

  name = "rename_onto_dir";
  fs_probe_rc(name, "mkdir_d", h2_pal_fs_mkdir(base_fs, d));
  fs_probe_rc(name, "create_f", fs_probe_create(f));
  fs_probe_rc(name, "rename", h2_pal_fs_rename(base_fs, f, d));
  fs_probe_cleanup(name);

  fs_probe_rc("rename_cross_dir", "rename",
              h2_pal_fs_rename(base_fs, "/data/x", "/dl/x"));

  h2_pal_fs_file_t *file = NULL;
  name = "rename_open";
  fs_probe_rc(name, "open_write", h2_pal_fs_open(
      base_fs, a, H2_PAL_FS_OPEN_WRITE_TRUNCATE, &file));
  fs_probe_rc(name, "rename", h2_pal_fs_rename(base_fs, a, b));
  fs_probe_rc(name, "close", h2_pal_fs_close(base_fs, file));
  file = NULL;
  fs_probe_stat(name, "stat_a", a);
  fs_probe_stat(name, "stat_b", b);
  fs_probe_cleanup(name);

  name = "remove_open";
  fs_probe_rc(name, "open_write", h2_pal_fs_open(
      base_fs, a, H2_PAL_FS_OPEN_WRITE_TRUNCATE, &file));
  fs_probe_rc(name, "remove", h2_pal_fs_remove(base_fs, a));
  fs_probe_rc(name, "close", h2_pal_fs_close(base_fs, file));
  file = NULL;
  fs_probe_cleanup(name);

  name = "open_twice";
  h2_pal_fs_file_t *second = NULL;
  h2_pal_fs_file_t *reader = NULL;
  fs_probe_rc(name, "open_write", h2_pal_fs_open(
      base_fs, a, H2_PAL_FS_OPEN_WRITE_TRUNCATE, &file));
  fs_probe_rc(name, "open_write_again", h2_pal_fs_open(
      base_fs, a, H2_PAL_FS_OPEN_WRITE_TRUNCATE, &second));
  fs_probe_rc(name, "open_read", h2_pal_fs_open(
      base_fs, a, H2_PAL_FS_OPEN_READ, &reader));
  if (second != NULL)
    fs_probe_rc(name, "close_second", h2_pal_fs_close(base_fs, second));
  if (reader != NULL)
    fs_probe_rc(name, "close_reader", h2_pal_fs_close(base_fs, reader));
  fs_probe_rc(name, "close", h2_pal_fs_close(base_fs, file));
  file = NULL;
  fs_probe_cleanup(name);

  const h2_pal_fs_open_mode_t modes[] = {
      H2_PAL_FS_OPEN_READ, H2_PAL_FS_OPEN_WRITE_TRUNCATE};
  const char *dir_names[] = {"open_dir_read", "open_dir_write"};
  for (size_t i = 0u; i < 2u; ++i) {
    name = dir_names[i];
    fs_probe_rc(name, "mkdir_d", h2_pal_fs_mkdir(base_fs, d));
    fs_probe_rc(name, "open", h2_pal_fs_open(base_fs, d, modes[i], &file));
    if (file != NULL) fs_probe_rc(name, "close", h2_pal_fs_close(base_fs, file));
    file = NULL;
    fs_probe_cleanup(name);
  }

  /* Two components keep the path boundary separate from the SDK's 130-unit
   * component limit. All entries stay inside the probe directory. */
  const char *prefix = "/data/h2-probe/length/";
  fs_probe_rc("path_191", "mkdir", h2_pal_fs_mkdir(base_fs, "/data/h2-probe/length"));
  char path[224];
  strcpy(path, prefix);
  size_t offset = strlen(prefix);
  memset(path + offset, 'p', 80u);
  path[offset + 80u] = '\0';
  fs_probe_rc("path_191", "mkdir_component", h2_pal_fs_mkdir(base_fs, path));
  path[offset + 80u] = '/';
  offset += 81u;
  for (size_t length = 191u; length <= 192u; ++length) {
    size_t public_length = length - (strlen("storage/sd0/C/") - 1u);
    memset(path + offset, 'n', public_length - offset);
    path[public_length] = '\0';
    fs_probe_name(length == 191u ? "path_191" : "path_192", path, 0);
  }
  fs_probe_cleanup("path_192");
  strcpy(path, "/data/h2-probe/");
  offset = strlen(path);
  /* 160 bytes keeps the translated path under 192 while exceeding the
   * SDK's 130-unit component limit, so the SDK answer is what is recorded. */
  memset(path + offset, 'c', 160u);
  path[offset + 160u] = '\0';
  fs_probe_name("component_160", path, 0);
  fs_probe_name("utf8_short", "/data/h2-probe/日志.txt", 1);
  fs_probe_name("utf8_long", "/data/h2-probe/日志-测试-非常长的文件名.txt", 1);
  fs_probe_cleanup("utf8_long");

  name = "sdk_frename_existing";
  const char *native_a = "storage/sd0/C/data/h2-probe/a";
  const char *native_b = "storage/sd0/C/data/h2-probe/b";
  FILE *sdk_a = fopen(native_a, "w+");
  fs_probe_native_open(name, "create_a", sdk_a);
  FILE *sdk_b = fopen(native_b, "w+");
  fs_probe_native_open(name, "create_b", sdk_b);
  if (sdk_b != NULL) fs_probe_native(name, "close_b", fclose(sdk_b));
  fs_probe_native(name, "rename", sdk_a == NULL ? -1 : frename(sdk_a, "b"));
  if (sdk_a != NULL) fs_probe_native(name, "close_a", fclose(sdk_a));
  sdk_a = fopen(native_a, "r");
  fs_probe_native(name, "delete_a", sdk_a == NULL ? -1 : fdelete(sdk_a));
  sdk_b = fopen(native_b, "r");
  fs_probe_native(name, "delete_b", sdk_b == NULL ? -1 : fdelete(sdk_b));
  fs_probe_cleanup(name);

  name = "sdk_fopen_dir_write";
  fs_probe_rc(name, "mkdir_d", h2_pal_fs_mkdir(base_fs, d));
  FILE *sdk_dir = fopen("storage/sd0/C/data/h2-probe/d", "w+");
  fs_probe_native_open(name, "open", sdk_dir);
  if (sdk_dir != NULL) fs_probe_native(name, "close", fclose(sdk_dir));
  fs_probe_cleanup(name);
  fs_probe_rc("cleanup", "remove", h2_pal_fs_remove(base_fs, "/data/h2-probe"));
}

#define TRACE_FS(name, parameters, arguments) \
  static int trace_fs_##name parameters { \
    (void)user; \
    trace("H2_PAL_FS op=" #name " phase=enter\r\n"); \
    int rc = base_fs->vtable->name arguments; \
    trace("H2_PAL_FS op=" #name " result=%d\r\n", rc); \
    return rc; \
  }
TRACE_FS(mkdir, (void *user, const char *path), (base_fs->user, path))
TRACE_FS(open, (void *user, const char *path, h2_pal_fs_open_mode_t mode, h2_pal_fs_file_t **file), (base_fs->user, path, mode, file))
TRACE_FS(read, (void *user, h2_pal_fs_file_t *file, void *data, size_t size, size_t *count), (base_fs->user, file, data, size, count))
TRACE_FS(write, (void *user, h2_pal_fs_file_t *file, const void *data, size_t size, size_t *count), (base_fs->user, file, data, size, count))
TRACE_FS(seek, (void *user, h2_pal_fs_file_t *file, uint64_t position), (base_fs->user, file, position))
TRACE_FS(close, (void *user, h2_pal_fs_file_t *file), (base_fs->user, file))
TRACE_FS(stat, (void *user, const char *path, h2_pal_fs_stat_t *value), (base_fs->user, path, value))
TRACE_FS(remove, (void *user, const char *path), (base_fs->user, path))
#undef TRACE_FS

static int get_running(void *user, h2_pal_power_boot_partition_t *out) {
  (void)user;
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  out->id = H2_JIELI_PARTITION_APP;
  out->flags = H2_PAL_POWER_BOOT_PARTITION_FLAG_BOOTABLE |
      H2_PAL_POWER_BOOT_PARTITION_FLAG_RUNNING | H2_PAL_POWER_BOOT_PARTITION_FLAG_APP;
  memcpy(out->name, "app", 4u);
  return H2_PAL_OK;
}

static int get_next(void *user, h2_pal_power_boot_partition_t *out) {
  int rc = get_running(user, out);
  if (rc != H2_PAL_OK) return rc;
  out->id = next_partition;
  out->flags = H2_PAL_POWER_BOOT_PARTITION_FLAG_BOOTABLE |
      H2_PAL_POWER_BOOT_PARTITION_FLAG_NEXT |
      (next_partition == H2_JIELI_PARTITION_LOADER ?
       H2_PAL_POWER_BOOT_PARTITION_FLAG_RECOVERY : H2_PAL_POWER_BOOT_PARTITION_FLAG_APP);
  snprintf(out->name, sizeof(out->name), "%s",
           next_partition == H2_JIELI_PARTITION_LOADER ? "h2loader" : "app");
  return H2_PAL_OK;
}

static int set_next(void *user, uint32_t partition) {
  (void)user;
  if (partition != H2_JIELI_PARTITION_LOADER) return H2_PAL_ERR_UNSUPPORTED;
  next_partition = partition;
  return H2_PAL_OK;
}

static int reboot(void *user, uint32_t reason) {
  (void)user;
  trace("H2_PAL_E2E_REBOOT reason=%u next=%u\r\n", reason, next_partition);
  os_time_dly(10u);
  if (next_partition != H2_JIELI_PARTITION_LOADER) return H2_PAL_ERR_UNSUPPORTED;
  int result = h2_jieli_app_loader_prepare_reboot(
      &loader_client.config, next_partition);
  if (result != H2_PAL_OK) return result;
  system_reset();
  return H2_PAL_OK;
}

static int start_commands(void) {
  static const h2_pal_power_vtable_t vtable = {
      .get_running_boot_partition = get_running,
      .get_next_boot_partition = get_next,
      .set_next_boot_partition = set_next,
      .reboot = reboot,
  };
  static const h2_pal_power_api_t power = {.vtable = &vtable};
  static h2_loader_app_client_config_t config;
  for (unsigned i = 0; !dev_online("sd0") && i < 500u; ++i) os_time_dly(1u);
  int rc = h2_jieli_ac791n_devkit_sd_fs_init(&loader_fs);
  if (rc == H2_PAL_OK) rc = h2_jieli_app_loader_config_init(
      &config, &loader_fs, &power, (h2_loader_memory_stats_api_t){0},
      H2_LOADER_CAPABILITY_UART);
  if (rc == H2_PAL_OK) rc = h2_loader_app_client_init(&loader_client, &config);
  if (rc == H2_PAL_OK) rc = h2_jieli_app_iostreamikcp_start(
      &loader_client, h2_jieli_wl82_platform_task_api(), h2_jieli_wl82_platform_mem_api());
  return rc;
}


/* Suites own Runtime through deferred cleanup; the final ledger needs no owner. */
static void run_suites(void *user) {
  (void)user;
  int result = H2_PAL_OK;
  static h2_pal_e2e_result_t reports[3];
  static const uint32_t suites[] = {
      H2_PAL_E2E_SUITE_FILESYSTEM, H2_PAL_E2E_SUITE_CORE,
      H2_PAL_E2E_SUITE_WIFI,
  };
  size_t passed = 0u;
  size_t failed = 0u;
  h2_runtime_config_t config;
  h2_runtime_t *runtime = NULL;
  trace("H2_PAL_E2E phase=runtime_begin\r\n");
  if (result == H2_PAL_OK)
    result = h2_jieli_ac791n_devkit_runtime_config(&config);
  static h2_pal_fs_vtable_t traced_vtable;
  static h2_pal_fs_api_t traced_fs;
  if (result == H2_PAL_OK && config.fs != NULL) {
    base_fs = config.fs;
    traced_vtable = *base_fs->vtable;
    traced_vtable.mkdir = trace_fs_mkdir;
    traced_vtable.open = trace_fs_open;
    traced_vtable.read = trace_fs_read;
    traced_vtable.write = trace_fs_write;
    traced_vtable.seek = trace_fs_seek;
    traced_vtable.close = trace_fs_close;
    traced_vtable.stat = trace_fs_stat;
    traced_vtable.remove = trace_fs_remove;
    traced_fs = *base_fs;
    traced_fs.vtable = &traced_vtable;
    config.fs = &traced_fs;
  }
  if (result == H2_PAL_OK && base_fs != NULL) {
    const char *parents[] = {"/dl", "/data"};
    for (size_t i = 0; i < sizeof(parents) / sizeof(parents[0]); ++i) {
      h2_pal_fs_stat_t st = {0};
      int rc = base_fs->vtable->stat(base_fs->user, parents[i], &st);
      trace("H2_PAL_FS parent path=%s stat=%d is_dir=%d\r\n",
            parents[i], rc, st.is_dir);
    }
  }
  if (result == H2_PAL_OK && base_fs != NULL) {
    h2_pal_fs_stat_t st = {0};
    const char *parent = "/data/h2-mkdir-missing-parent";
    int before = base_fs->vtable->stat(base_fs->user, parent, &st);
    if (before == H2_PAL_ERR_NOT_FOUND) {
      int made = base_fs->vtable->mkdir(
          base_fs->user, "/data/h2-mkdir-missing-parent/child");
      int after = base_fs->vtable->stat(base_fs->user, parent, &st);
      trace("H2_PAL_FS missing_parent before=%d mkdir=%d after=%d\r\n",
            before, made, after);
    } else {
      trace("H2_PAL_FS missing_parent skipped stat=%d\r\n", before);
    }
  }
  if (result == H2_PAL_OK && base_fs != NULL) run_fs_probes();
  if (result == H2_PAL_OK) result = h2_runtime_init(&config, &runtime);
  trace("H2_PAL_E2E phase=runtime result=%d\r\n", result);
  if (result == H2_PAL_OK) {
    for (size_t s = 0; s < sizeof(suites) / sizeof(suites[0]); ++s) {
      const h2_pal_e2e_config_t suite = {.suite_mask = suites[s]};
      h2_pal_e2e_result_t *report = &reports[s];
      trace("H2_PAL_E2E phase=suite_begin suite=%u\r\n", suites[s]);
      int suite_result = h2_pal_e2e_run(runtime, &suite, report);
      if (result == H2_PAL_OK) result = suite_result;
      for (size_t i = 0; i < report->case_count; ++i) {
        trace("H2_PAL_E2E suite=%u case=%u result=%d\r\n", suites[s],
               (unsigned)report->cases[i].case_id, report->cases[i].result);
      }
      passed += report->passed;
      failed += report->failed;
      if (report->retained_cleanup != NULL) break;
    }
  }
  /* Retained Task cleanup borrows the Runtime. Make progress until all
   * workers release it, then keep only the value-only ledger below. */
  for (size_t s = 0; s < sizeof(suites) / sizeof(suites[0]); ++s) {
    while (reports[s].retained_cleanup != NULL) {
      (void)h2_pal_e2e_cleanup(runtime, &reports[s]);
      if (reports[s].retained_cleanup != NULL) os_time_dly(1u);
    }
  }
  if (runtime != NULL) h2_runtime_deinit(runtime);
  /* Deliberately leave this diagnostic App unconfirmed: a reset must recover
   * to Loader even if a later PAL test hangs. UART commands remain available. */
  for (;;) {
    /* Reprint the ledger because host reconnect can miss early boot output. */
    for (size_t s = 0; s < sizeof(suites) / sizeof(suites[0]); ++s) {
      for (size_t i = 0; i < reports[s].case_count; ++i) {
        trace("H2_PAL_E2E suite=%u case=%u result=%d\r\n", suites[s],
               (unsigned)reports[s].cases[i].case_id,
               reports[s].cases[i].result);
      }
    }
    trace("H2_PAL_E2E result=%d passed=%u failed=%u\r\n", result,
           (unsigned)passed, (unsigned)failed);
    os_time_dly(500u);
  }
}

void app_main(void) {
  int result = start_commands();
  trace("H2_PAL_E2E phase=commands result=%d\r\n", result);
  static h2_pal_task_t *runner;
  const h2_pal_task_options_t options = {.name = h2_pal_e2e_runner_task_name};
  if (result == H2_PAL_OK) {
    result = h2_pal_task_start(h2_jieli_wl82_platform_task_api(), &options,
                               run_suites, NULL, &runner);
  }
  trace("H2_PAL_E2E phase=runner result=%d\r\n", result);
  /* SDK app_core must return to dispatch events; the runner owns the ledger. */
  if (result != H2_PAL_OK) system_reset();
}
