#include "h2_desktop_platform.h"
#include "h2_lua.h"
#include "h2_lua_job.h"
#include "h2_lua_module.h"
#include "h2_pal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* In-memory PAL Filesystem with one flat path table. mkdir and create need an
 * existing parent directory, rename replaces its target, and write, rename and
 * sync can be made to fail on a chosen call. */

#define FAKE_FS_PATH_MAX 192u
#define FAKE_FS_ENTRY_MAX 64u
#define FAKE_FS_DATA_MAX 256u

typedef struct fake_fs_entry {
  int used;
  int is_dir;
  char path[FAKE_FS_PATH_MAX];
  uint8_t data[FAKE_FS_DATA_MAX];
  size_t size;
} fake_fs_entry_t;

typedef struct fake_fs_file {
  fake_fs_entry_t *entry;
  size_t offset;
  int writable;
} fake_fs_file_t;

typedef struct fake_fs {
  fake_fs_entry_t entries[FAKE_FS_ENTRY_MAX];
  fake_fs_file_t files[4];
  /* A nonzero countdown fails the call on which it reaches zero. */
  int fail_write_in;
  int fail_rename_in;
  int fail_sync_in;
  size_t rename_count;
} fake_fs_t;

static fake_fs_t s_fs;

static fake_fs_entry_t *fake_find(const char *path) {
  for (size_t i = 0u; i < FAKE_FS_ENTRY_MAX; ++i) {
    if (s_fs.entries[i].used && strcmp(s_fs.entries[i].path, path) == 0) {
      return &s_fs.entries[i];
    }
  }
  return NULL;
}

static int fake_parent_exists(const char *path) {
  char parent[FAKE_FS_PATH_MAX];
  const char *slash = strrchr(path, '/');
  fake_fs_entry_t *entry;
  if (slash == NULL || slash == path) {
    return 1;
  }
  memcpy(parent, path, (size_t)(slash - path));
  parent[slash - path] = '\0';
  entry = fake_find(parent);
  return entry != NULL && entry->is_dir;
}

static fake_fs_entry_t *fake_create(const char *path, int is_dir) {
  assert(strlen(path) < FAKE_FS_PATH_MAX);
  for (size_t i = 0u; i < FAKE_FS_ENTRY_MAX; ++i) {
    if (!s_fs.entries[i].used) {
      fake_fs_entry_t *entry = &s_fs.entries[i];
      memset(entry, 0, sizeof(*entry));
      entry->used = 1;
      entry->is_dir = is_dir;
      memcpy(entry->path, path, strlen(path) + 1u);
      return entry;
    }
  }
  return NULL;
}

static int fake_countdown(int *counter) {
  if (*counter > 0 && --*counter == 0) {
    return 1;
  }
  return 0;
}

static int fake_mkdir(void *user, const char *path) {
  fake_fs_entry_t *entry = fake_find(path);
  (void)user;
  if (entry != NULL) {
    return entry->is_dir ? H2_PAL_OK : H2_PAL_ERR_IO;
  }
  if (!fake_parent_exists(path)) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  return fake_create(path, 1) != NULL ? H2_PAL_OK : H2_PAL_ERR_NO_SPACE;
}

static int fake_open(void *user, const char *path, h2_pal_fs_open_mode_t mode,
                     h2_pal_fs_file_t **out_file) {
  fake_fs_entry_t *entry = fake_find(path);
  fake_fs_file_t *file = NULL;
  (void)user;
  for (size_t i = 0u; i < 4u; ++i) {
    if (s_fs.files[i].entry == NULL) {
      file = &s_fs.files[i];
      break;
    }
  }
  assert(file != NULL);
  if (mode == H2_PAL_FS_OPEN_READ) {
    if (entry == NULL || entry->is_dir) {
      return H2_PAL_ERR_NOT_FOUND;
    }
  } else {
    if (entry != NULL && entry->is_dir) {
      return H2_PAL_ERR_IO;
    }
    if (entry == NULL) {
      if (!fake_parent_exists(path)) {
        return H2_PAL_ERR_NOT_FOUND;
      }
      entry = fake_create(path, 0);
      if (entry == NULL) {
        return H2_PAL_ERR_NO_SPACE;
      }
    }
    entry->size = 0u;
  }
  *file = (fake_fs_file_t){
      .entry = entry,
      .writable = mode == H2_PAL_FS_OPEN_WRITE_TRUNCATE,
  };
  *out_file = (h2_pal_fs_file_t *)file;
  return H2_PAL_OK;
}

static int fake_read(void *user, h2_pal_fs_file_t *handle, void *data,
                     size_t length, size_t *out_read) {
  fake_fs_file_t *file = (fake_fs_file_t *)handle;
  size_t remaining = file->entry->size - file->offset;
  size_t count = length < remaining ? length : remaining;
  (void)user;
  /* Short reads exercise the read loop. */
  count = count > 7u ? 7u : count;
  memcpy(data, file->entry->data + file->offset, count);
  file->offset += count;
  *out_read = count;
  return H2_PAL_OK;
}

static int fake_write(void *user, h2_pal_fs_file_t *handle, const void *data,
                      size_t length, size_t *out_written) {
  fake_fs_file_t *file = (fake_fs_file_t *)handle;
  size_t count = length > 5u ? 5u : length;
  (void)user;
  *out_written = 0u;
  if (!file->writable) {
    return H2_PAL_ERR_IO;
  }
  if (fake_countdown(&s_fs.fail_write_in)) {
    return H2_PAL_ERR_IO;
  }
  if (file->entry->size + count > FAKE_FS_DATA_MAX) {
    return H2_PAL_ERR_NO_SPACE;
  }
  memcpy(file->entry->data + file->entry->size, data, count);
  file->entry->size += count;
  *out_written = count;
  return H2_PAL_OK;
}

static int fake_sync(void *user, h2_pal_fs_file_t *handle) {
  (void)user;
  (void)handle;
  return fake_countdown(&s_fs.fail_sync_in) ? H2_PAL_ERR_IO : H2_PAL_OK;
}

static int fake_close(void *user, h2_pal_fs_file_t *handle) {
  fake_fs_file_t *file = (fake_fs_file_t *)handle;
  (void)user;
  file->entry = NULL;
  return H2_PAL_OK;
}

static int fake_stat(void *user, const char *path, h2_pal_fs_stat_t *out_stat) {
  fake_fs_entry_t *entry = fake_find(path);
  (void)user;
  if (entry == NULL) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  *out_stat = (h2_pal_fs_stat_t){.size = entry->size, .is_dir = entry->is_dir};
  return H2_PAL_OK;
}

static int fake_remove(void *user, const char *path) {
  fake_fs_entry_t *entry = fake_find(path);
  (void)user;
  if (entry == NULL) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  entry->used = 0;
  return H2_PAL_OK;
}

static int fake_rename(void *user, const char *old_path, const char *new_path) {
  fake_fs_entry_t *source = fake_find(old_path);
  fake_fs_entry_t *target = fake_find(new_path);
  (void)user;
  if (fake_countdown(&s_fs.fail_rename_in)) {
    return H2_PAL_ERR_IO;
  }
  if (source == NULL) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  if (target != NULL) {
    target->used = 0;
  }
  memcpy(source->path, new_path, strlen(new_path) + 1u);
  s_fs.rename_count++;
  return H2_PAL_OK;
}

static const h2_pal_fs_vtable_t s_fake_fs_vtable = {
    .mkdir = fake_mkdir,
    .open = fake_open,
    .read = fake_read,
    .write = fake_write,
    .sync = fake_sync,
    .close = fake_close,
    .stat = fake_stat,
    .remove = fake_remove,
    .rename = fake_rename,
};

static const h2_pal_fs_api_t s_fake_fs_api = {
    .user = &s_fs,
    .vtable = &s_fake_fs_vtable,
};

static void fake_reset(void) {
  memset(&s_fs, 0, sizeof(s_fs));
  /* The mount point exists; the Lua root below it does not yet. */
  assert(fake_create("/data", 1) != NULL);
}

static void fake_put(const char *path, const char *text) {
  fake_fs_entry_t *entry = fake_find(path);
  if (entry == NULL) {
    entry = fake_create(path, 0);
  }
  assert(entry != NULL && strlen(text) <= FAKE_FS_DATA_MAX);
  memcpy(entry->data, text, strlen(text));
  entry->size = strlen(text);
}

static int fake_file_is(const char *path, const char *text) {
  fake_fs_entry_t *entry = fake_find(path);
  return entry != NULL && !entry->is_dir && entry->size == strlen(text) &&
         memcmp(entry->data, text, entry->size) == 0;
}

/* Every path in the fs is the mount point or lies under the storage root. */
static void assert_confined_to(const char *root) {
  size_t root_length = strlen(root);
  for (size_t i = 0u; i < FAKE_FS_ENTRY_MAX; ++i) {
    const char *path = s_fs.entries[i].path;
    if (!s_fs.entries[i].used || strcmp(path, "/data") == 0) {
      continue;
    }
    assert(strncmp(path, root, root_length) == 0 &&
           (path[root_length] == '\0' || path[root_length] == '/'));
  }
}

static void assert_no_temp_files(void) {
  for (size_t i = 0u; i < FAKE_FS_ENTRY_MAX; ++i) {
    if (s_fs.entries[i].used) {
      assert(strstr(s_fs.entries[i].path, ".tmp") == NULL);
    }
  }
}

static h2_runtime_t *create_runtime(void) {
  h2_runtime_config_t config = {
      .board = "test",
      .target = "desktop",
      .chip = "host",
      .firmware_info = h2_pal_unsupported_firmware_info_api(),
      .mem = h2_desktop_platform_default_allocator(),
      .log = h2_desktop_platform_log_api(),
      .time = h2_desktop_platform_time_api(),
      .timer = h2_pal_unsupported_timer_api(),
      .task = h2_desktop_platform_task_api(),
      .queue = h2_desktop_platform_queue_api(),
      .sync = h2_desktop_platform_sync_api(),
      .fs = h2_pal_unsupported_fs_api(),
      .disk = h2_pal_unsupported_disk_api(),
      .pref = h2_pal_unsupported_pref_api(),
      .crypto = h2_pal_unsupported_crypto_api(),
      .http = h2_pal_unsupported_http_api(),
      .net = h2_pal_unsupported_net_api(),
      .netif = h2_pal_unsupported_netif_api(),
      .mqtt = h2_pal_unsupported_mqtt_api(),
      .webrtc = h2_pal_unsupported_webrtc_api(),
      .wifi_sta = h2_pal_unsupported_wifi_sta_api(),
      .wifi_ap = h2_pal_unsupported_wifi_ap_api(),
      .wifi_csi = h2_pal_unsupported_wifi_csi_api(),
      .wifi_settings = h2_pal_unsupported_wifi_settings_api(),
      .ble_host = h2_pal_unsupported_ble_host_api(),
      .modem = h2_pal_unsupported_modem_api(),
      .power = h2_pal_unsupported_power_api(),
      .display = h2_pal_unsupported_display_api(),
      .audio = h2_pal_unsupported_audio_api(),
      .audio_decoder = h2_pal_unsupported_audio_decoder_api(),
      .periph = h2_pal_unsupported_periph_api(),
      .button = h2_pal_unsupported_button_api(),
      .touch = h2_pal_unsupported_touch_api(),
      .buzzer = h2_pal_unsupported_buzzer_api(),
      .nfc = h2_pal_unsupported_nfc_api(),
      .nfc_card_emulation = h2_pal_unsupported_nfc_card_emulation_api(),
      .imu = h2_pal_unsupported_imu_api(),
      .gpio_irq = h2_pal_unsupported_gpio_irq_api(),
      .led = h2_pal_unsupported_led_api(),
      .switch_api = h2_pal_unsupported_switch_api(),
      .pwm_switch = h2_pal_unsupported_pwm_switch_api(),
      .input = h2_pal_unsupported_input_api(),
      .system_event = h2_pal_unsupported_system_event_api(),
      .video_decoder = h2_pal_unsupported_video_decoder_api(),
  };
  h2_runtime_t *runtime = NULL;
  assert(h2_runtime_init(&config, &runtime) == H2_PAL_OK);
  return runtime;
}

static h2_lua_host_config_t host_config(h2_runtime_t *runtime,
                                        const h2_pal_fs_api_t *fs) {
  return (h2_lua_host_config_t){
      .runtime = runtime,
      .worker_count = 1u,
      .max_jobs = 2u,
      .vm_memory_limit_bytes = 256u * 1024u,
      .storage =
          {
              .fs = fs,
              .root = "/data/lua",
              .app_quota_bytes = 64u,
              .app_max_files = 3u,
          },
  };
}

static h2_lua_host_t *create_host(h2_runtime_t *runtime,
                                  const h2_pal_fs_api_t *fs) {
  h2_lua_host_config_t config = host_config(runtime, fs);
  h2_lua_host_t *host = NULL;
  assert(h2_lua_host_create(&config, &host) == H2_PAL_OK);
  assert(h2_lua_host_start(host) == H2_PAL_OK);
  return host;
}

/* Runs one script to completion and returns its final status. */
static h2_lua_job_status_t run(h2_lua_host_t *host, const char *app_id,
                               const char *script) {
  h2_lua_job_id_t job_id = H2_LUA_JOB_ID_NONE;
  h2_lua_job_status_t status;
  assert(h2_lua_job_submit_text(host, app_id, "@storage.lua",
                                (const uint8_t *)script, strlen(script), NULL,
                                0u, &job_id) == H2_PAL_OK);
  for (size_t step = 0u; step < 5000u; ++step) {
    assert(h2_lua_job_get_status(host, job_id, &status) == H2_PAL_OK);
    if (status.state != H2_LUA_JOB_QUEUED &&
        status.state != H2_LUA_JOB_RUNNING &&
        status.state != H2_LUA_JOB_WAITING) {
      assert(h2_lua_job_release(host, job_id) == H2_PAL_OK);
      return status;
    }
    assert(h2_lua_host_step(host) == H2_PAL_OK);
    (void)h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 1u);
  }
  assert(!"storage job did not finish");
  return status;
}

static void expect_ok(h2_lua_host_t *host, const char *app_id,
                      const char *script) {
  h2_lua_job_status_t status = run(host, app_id, script);
  if (status.state != H2_LUA_JOB_SUCCEEDED ||
      strcmp(status.message, "ok") != 0) {
    fprintf(stderr, "storage script failed (%d): %s\n", (int)status.state,
            status.message);
    assert(0);
  }
}

static void test_unconfigured(h2_runtime_t *runtime) {
  static const char script[] =
      "local s=require('storage');"
      "local ok,err=s.write_file('score','1');"
      "assert(ok==nil and err=='storage: unavailable',err);"
      "assert(s.read_file('score')==nil);"
      "assert(s.exists('score')==false);"
      "assert(s.get_root_dir()==nil);"
      "assert(s.listdir()==nil);"
      "assert(s.get_free_space()==nil);"
      "assert(s.join_path('a','b')=='a/b');"
      "return 'ok'";
  h2_lua_host_t *host = create_host(runtime, NULL);
  expect_ok(host, "game", script);
  h2_lua_host_destroy(host);

  /* A configured Host still gives an anonymous job no storage. */
  fake_reset();
  host = create_host(runtime, &s_fake_fs_api);
  expect_ok(host, NULL, script);
  h2_lua_host_destroy(host);
  assert(fake_find("/data/lua") == NULL);
}

static void test_round_trip(h2_runtime_t *runtime) {
  h2_lua_host_t *host;
  fake_reset();
  host = create_host(runtime, &s_fake_fs_api);
  expect_ok(host, "game",
            "local s=require('storage');"
            "assert(s.get_root_dir()=='');"
            "local path=s.join_path(s.get_root_dir(),'score.json');"
            "assert(path=='score.json',path);"
            "assert(s.join_path('/a/','/b/','c')=='/a/b/c');"
            "assert(s.write_file(path,'{\"best\":42}')==true);"
            "assert(s.read_file(path)=='{\"best\":42}');"
            "assert(s.exists(path)==true and s.exists('nope')==false);"
            "local st=s.stat(path);"
            "assert(st.type=='file' and st.size==11);"
            "assert(s.write_file('empty','')==true);"
            "assert(s.read_file('empty')=='');"
            "local bin=string.char(0,1,2,255);"
            "assert(s.write_file('bin',bin)==true and s.read_file('bin')==bin);"
            "local list=s.listdir('');assert(#list==3);"
            "local seen={};for _,e in ipairs(list) do "
            "assert(e.type=='file');seen[e.name]=e.size end;"
            "assert(seen['score.json']==11 and seen.empty==0 and seen.bin==4);"
            "local space=s.get_free_space();"
            "assert(space.total==64 and space.used==15 and space.free==49);"
            "assert(s.rename('bin','raw')==true and not s.exists('bin'));"
            "assert(s.read_file('raw')==bin);"
            "assert(s.remove('raw')==true and s.exists('raw')==false);"
            "local ok,err=s.remove('raw');assert(ok==nil and "
            "err=='storage: not found');"
            "ok,err=s.read_file('raw');assert(ok==nil and "
            "err=='storage: not found');"
            "return 'ok'");
  assert(fake_file_is("/data/lua/game/score.json", "{\"best\":42}"));
  assert(fake_file_is("/data/lua/game/empty", ""));
  assert(fake_file_is("/data/lua/game/.index", "score.json\nempty\n"));
  assert_no_temp_files();

  /* Files survive the Host and a new job of the same app sees them. */
  h2_lua_host_destroy(host);
  host = create_host(runtime, &s_fake_fs_api);
  expect_ok(host, "game",
            "local s=require('storage');"
            "assert(s.read_file('score.json')=='{\"best\":42}');"
            "assert(#s.listdir()==2);"
            "assert(s.rename('empty','score.json')==true);"
            "assert(s.read_file('score.json')=='');"
            "assert(#s.listdir()==1);"
            "return 'ok'");
  assert(fake_file_is("/data/lua/game/.index", "score.json\n"));
  h2_lua_host_destroy(host);
}

static void test_scoping(h2_runtime_t *runtime) {
  h2_lua_host_t *host;
  fake_reset();
  host = create_host(runtime, &s_fake_fs_api);
  expect_ok(host, "alpha",
            "local s=require('storage');"
            "assert(s.write_file('score','100'));return 'ok'");
  expect_ok(host, "beta",
            "local s=require('storage');"
            "assert(s.exists('score')==false);"
            "assert(#s.listdir()==0);"
            "local ok,err=s.read_file('score');"
            "assert(ok==nil and err=='storage: not found');"
            "assert(s.write_file('score','7'));"
            "assert(s.get_free_space().used==1);"
            "return 'ok'");
  expect_ok(host, "alpha",
            "local s=require('storage');"
            "assert(s.read_file('score')=='100');return 'ok'");
  assert(fake_file_is("/data/lua/alpha/score", "100"));
  assert(fake_file_is("/data/lua/beta/score", "7"));
  assert_confined_to("/data/lua");
  h2_lua_host_destroy(host);
}

static void test_traversal(h2_runtime_t *runtime) {
  h2_lua_host_t *host;
  fake_reset();
  fake_put("/data/secret", "keep");
  host = create_host(runtime, &s_fake_fs_api);
  expect_ok(host, "alpha",
            "local s=require('storage');"
            "local bad={'','/data/secret','../secret','../../secret','..',"
            "'.','a/b','./a','.index','.data.tmp','.hidden','Upper',"
            "'a\\\\b','a:b','a b','a\\0b',string.rep('x',33)};"
            "for _,name in ipairs(bad) do "
            "local ok,err=s.write_file(name,'x');"
            "assert(ok==nil and err=='storage: invalid name',name);"
            "ok,err=s.read_file(name);"
            "assert(ok==nil and err=='storage: invalid name',name);"
            "assert(s.exists(name)==false,name);"
            "assert(s.stat(name)==nil and s.remove(name)==nil,name);"
            "assert(s.rename(name,'ok')==nil and s.rename('ok',name)==nil);"
            "end;"
            "local ok,err=s.listdir('..');"
            "assert(ok==nil and err=='storage: invalid name');"
            "ok,err=s.listdir('sub');"
            "assert(ok==nil and err=='storage: not found');"
            "assert(s.write_file(string.rep('x',32),'x'));"
            "return 'ok'");
  assert(fake_file_is("/data/secret", "keep"));
  h2_lua_host_destroy(host);

  /* The same rules apply to the app id the Host submits. */
  host = create_host(runtime, &s_fake_fs_api);
  {
    static const char *const bad_ids[] = {
        "",        ".",     "..",
        "../x",    "a/b",   "/abs",
        ".hidden", "Upper", "123456789012345678901234567890123",
    };
    static const uint8_t script[] = "return 1";
    for (size_t i = 0u; i < sizeof(bad_ids) / sizeof(bad_ids[0]); ++i) {
      h2_lua_job_id_t job_id = H2_LUA_JOB_ID_NONE;
      assert(h2_lua_job_submit_text(host, bad_ids[i], "@bad.lua", script,
                                    sizeof(script) - 1u, NULL, 0u,
                                    &job_id) == H2_PAL_ERR_INVALID_ARG);
      assert(job_id == H2_LUA_JOB_ID_NONE);
    }
  }
  h2_lua_host_destroy(host);
}

static void test_quota(h2_runtime_t *runtime) {
  h2_lua_host_t *host;
  fake_reset();
  host = create_host(runtime, &s_fake_fs_api);
  expect_ok(host, "game",
            "local s=require('storage');"
            "local ok,err=s.write_file('big',string.rep('x',65));"
            "assert(ok==nil and err=='storage: quota exceeded',err);"
            "assert(s.exists('big')==false);"
            "assert(s.write_file('a',string.rep('a',40)));"
            "ok,err=s.write_file('b',string.rep('b',30));"
            "assert(ok==nil and err=='storage: quota exceeded',err);"
            "assert(s.exists('b')==false);"
            /* Replacing a file only counts its new size. */
            "assert(s.write_file('a',string.rep('a',60)));"
            "assert(s.write_file('b',string.rep('b',4)));"
            "assert(s.get_free_space().free==0);"
            "ok,err=s.write_file('b',string.rep('b',5));"
            "assert(ok==nil and err=='storage: quota exceeded');"
            "assert(s.read_file('b')=='bbbb');"
            "assert(s.write_file('a',''));"
            "assert(s.write_file('c','c'));"
            "ok,err=s.write_file('d','d');"
            "assert(ok==nil and err=='storage: too many files',err);"
            "assert(s.write_file('c','cc'));"
            "assert(s.remove('c'));"
            "assert(s.write_file('d','d'));"
            "assert(#s.listdir()==3);"
            "return 'ok'");
  assert(fake_find("/data/lua/game/big") == NULL);
  assert(fake_find("/data/lua/game/c") == NULL);
  assert_no_temp_files();
  h2_lua_host_destroy(host);
}

static void test_atomic_write(h2_runtime_t *runtime) {
  static const char check_v1[] =
      "local s=require('storage');"
      "local ok,err=s.write_file('save','version-two-longer');"
      "assert(ok==nil and err=='storage: io error',err);"
      "assert(s.read_file('save')=='version-one');"
      "assert(s.stat('save').size==11);"
      "return 'ok'";
  h2_lua_host_t *host;
  fake_reset();
  host = create_host(runtime, &s_fake_fs_api);
  expect_ok(host, "game",
            "assert(require('storage').write_file('save','version-one'));"
            "return 'ok'");
  assert(s_fs.rename_count == 2u);

  /* Failing the second chunk leaves a partial temp file, never a partial
   * target. */
  s_fs.fail_write_in = 2;
  expect_ok(host, "game", check_v1);
  s_fs.fail_sync_in = 1;
  expect_ok(host, "game", check_v1);
  s_fs.fail_rename_in = 1;
  expect_ok(host, "game", check_v1);
  assert(fake_file_is("/data/lua/game/save", "version-one"));
  assert_no_temp_files();

  /* A new name whose content fails to land is not left in the index. The
   * index "save\nfresh\n" takes three 5-byte writes, the content two. */
  s_fs.fail_write_in = 5;
  expect_ok(host, "game",
            "local s=require('storage');"
            "local ok,err=s.write_file('fresh','0123456789');"
            "assert(ok==nil and err=='storage: io error',err);"
            "assert(s.exists('fresh')==false);"
            "assert(#s.listdir()==1);"
            "return 'ok'");
  assert(fake_find("/data/lua/game/fresh") == NULL);
  assert(fake_file_is("/data/lua/game/.index", "save\n"));

  /* A failed index update keeps the previous content and index. */
  s_fs.fail_rename_in = 1;
  expect_ok(host, "game",
            "local s=require('storage');"
            "local ok,err=s.write_file('other','x');"
            "assert(ok==nil and err=='storage: io error',err);"
            "assert(s.exists('other')==false);"
            "return 'ok'");
  assert(fake_file_is("/data/lua/game/.index", "save\n"));
  assert_no_temp_files();
  h2_lua_host_destroy(host);
}

static void test_recovery(h2_runtime_t *runtime) {
  h2_lua_host_t *host;
  fake_reset();
  assert(fake_create("/data/lua", 1) != NULL);
  assert(fake_create("/data/lua/game", 1) != NULL);
  /* Left behind by an interrupted write and a hand-edited index. */
  fake_put("/data/lua/game/.index",
           "ghost\nsave\nsave\n../evil\nUPPER\n\nsave2");
  fake_put("/data/lua/game/save", "kept");
  fake_put("/data/lua/game/save2", "orphan");
  fake_put("/data/lua/game/.data.tmp", "partial");
  host = create_host(runtime, &s_fake_fs_api);
  expect_ok(host, "game",
            "local s=require('storage');"
            "local list=s.listdir();"
            "assert(#list==1 and list[1].name=='save' and list[1].size==4);"
            "assert(s.exists('ghost')==false and s.exists('save2')==false);"
            "assert(s.get_free_space().used==4);"
            "assert(s.write_file('next','n'));"
            "return 'ok'");
  assert(fake_file_is("/data/lua/game/.index", "save\nnext\n"));
  h2_lua_host_destroy(host);
}

static int noop_module(void *lua_state, void *user) {
  (void)lua_state;
  (void)user;
  return 0;
}

static void test_config(h2_runtime_t *runtime) {
  h2_pal_fs_vtable_t no_rename = s_fake_fs_vtable;
  h2_pal_fs_api_t no_rename_api = {.user = &s_fs, .vtable = &no_rename};
  char long_root[H2_LUA_STORAGE_ROOT_MAX + 2u];
  h2_lua_host_config_t config;
  h2_lua_host_t *host = NULL;

  config = host_config(runtime, &s_fake_fs_api);
  config.storage.root = NULL;
  assert(h2_lua_host_create(&config, &host) == H2_PAL_ERR_INVALID_ARG);
  config.storage.root = "";
  assert(h2_lua_host_create(&config, &host) == H2_PAL_ERR_INVALID_ARG);
  config.storage.root = "/data/lua/";
  assert(h2_lua_host_create(&config, &host) == H2_PAL_ERR_INVALID_ARG);
  memset(long_root, 'r', sizeof(long_root) - 1u);
  long_root[sizeof(long_root) - 1u] = '\0';
  config.storage.root = long_root;
  assert(h2_lua_host_create(&config, &host) == H2_PAL_ERR_INVALID_ARG);
  long_root[sizeof(long_root) - 2u] = '\0';
  config.storage.root = long_root;
  assert(h2_lua_host_create(&config, &host) == H2_PAL_OK);
  h2_lua_host_destroy(host);
  host = NULL;

  config = host_config(runtime, &s_fake_fs_api);
  config.storage.app_max_files = H2_LUA_STORAGE_MAX_FILES + 1u;
  assert(h2_lua_host_create(&config, &host) == H2_PAL_ERR_INVALID_ARG);
  no_rename.rename = NULL;
  config = host_config(runtime, &no_rename_api);
  assert(h2_lua_host_create(&config, &host) == H2_PAL_ERR_UNSUPPORTED);
  assert(host == NULL);

  /* Zero limits select the defaults: 16 files and 64 KiB. */
  fake_reset();
  config = host_config(runtime, &s_fake_fs_api);
  config.storage.app_quota_bytes = 0u;
  config.storage.app_max_files = 0u;
  assert(h2_lua_host_create(&config, &host) == H2_PAL_OK);
  assert(h2_lua_register_module(host, "storage", noop_module, NULL) !=
         H2_PAL_OK);
  assert(h2_lua_host_start(host) == H2_PAL_OK);
  expect_ok(host, "game",
            "local s=require('storage');"
            "assert(s.get_free_space().total==65536);"
            "for i=1,16 do assert(s.write_file('f'..i,'x')) end;"
            "local ok,err=s.write_file('f17','x');"
            "assert(ok==nil and err=='storage: too many files');"
            "return 'ok'");
  h2_lua_host_destroy(host);
}

int main(void) {
  h2_runtime_t *runtime = create_runtime();
  test_unconfigured(runtime);
  test_round_trip(runtime);
  test_scoping(runtime);
  test_traversal(runtime);
  test_quota(runtime);
  test_atomic_write(runtime);
  test_recovery(runtime);
  test_config(runtime);
  h2_runtime_deinit(runtime);
  return 0;
}
