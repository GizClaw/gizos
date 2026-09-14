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
#define FAKE_FS_DATA_MAX (128u * 1024u)

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
  int grow_stat_calls;
  int fail_no_space_in;
  int fail_read_in;
  int fail_close_in;
  int fail_remove_in;
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
  if (fake_countdown(&s_fs.fail_read_in))
    return H2_PAL_ERR_IO;
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
  if (fake_countdown(&s_fs.fail_no_space_in))
    return H2_PAL_ERR_NO_SPACE;
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
  return fake_countdown(&s_fs.fail_close_in) ? H2_PAL_ERR_IO : H2_PAL_OK;
}

static int fake_stat(void *user, const char *path, h2_pal_fs_stat_t *out_stat) {
  fake_fs_entry_t *entry = fake_find(path);
  (void)user;
  if (entry == NULL) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  *out_stat = (h2_pal_fs_stat_t){.size = entry->size, .is_dir = entry->is_dir};
  /* Emulate a writer growing the file between sizing and the next read. */
  if (s_fs.grow_stat_calls > 0 && strstr(path, "/.kv") != NULL) {
    --s_fs.grow_stat_calls;
    ++entry->size;
  }
  return H2_PAL_OK;
}

static int fake_remove(void *user, const char *path) {
  fake_fs_entry_t *entry = fake_find(path);
  (void)user;
  if (entry == NULL) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  if (fake_countdown(&s_fs.fail_remove_in))
    return H2_PAL_ERR_IO;
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
              .app_quota_bytes = 65536u,
              .app_max_files = 16u,
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

static void test_roundtrip(h2_runtime_t *runtime) {
  fake_reset();
  h2_lua_host_t *host = create_host(runtime, &s_fake_fs_api);
  expect_ok(
      host, "game",
      "local k=require('kv'); assert(k.get('x')==nil and not k.exists('x'));"
      "assert(#k.keys()==0 and k.remove('x')==false); return 'ok'");
  assert(fake_find("/data/lua") == NULL);
  expect_ok(
      host, "game",
      "local k=require('kv');"
      "assert(k.set('z','a'..string.char(0,255))); assert(k.set('empty',''));"
      "assert(k.set('flag',false)); assert(k.set('int',math.maxinteger));"
      "assert(k.set('min',math.mininteger)); assert(k.set('float',1.0));"
      "assert(k.set('negative_zero',-0.0)); assert(k.set('utf8','小明'));"
      "assert(k.set('flag',true)); assert(k.set('flag',false));"
      "assert(k.get('flag')==false and k.exists('flag'));"
      "assert(k.get('z')=='a'..string.char(0,255)); assert(k.get('empty')=='');"
      "assert(k.get('int')==math.maxinteger and k.get('min')==math.mininteger);"
      "assert(math.type(k.get('float'))=='float' and "
      "math.type(k.get('int'))=='integer');"
      "assert(1/k.get('negative_zero')==-math.huge);"
      "local keys=k.keys(); for i=2,#keys do assert(keys[i-1]<keys[i]) end;"
      "assert(k.remove('z') and not k.remove('z')); return 'ok'");
  expect_ok(host, "other",
            "local k=require('kv'); assert(#k.keys()==0); return 'ok'");
  h2_lua_host_destroy(host);
  host = create_host(runtime, &s_fake_fs_api);
  expect_ok(host, "game",
            "local k=require('kv'); assert(k.get('utf8')=='小明');"
            "assert(k.get('int')==math.maxinteger);"
            "for _,key in ipairs(k.keys()) do assert(k.remove(key)) end;"
            "assert(#k.keys()==0); return 'ok'");
  assert(fake_find("/data/lua/game/.kv") == NULL);
  assert_confined_to("/data/lua");
  assert_no_temp_files();
  h2_lua_host_destroy(host);
}

static void test_arguments(h2_runtime_t *runtime) {
  fake_reset();
  h2_lua_host_t *host = create_host(runtime, &s_fake_fs_api);
  expect_ok(
      host, "game",
      "local k=require('kv');"
      "for _,key in "
      "ipairs({'','A','.x','../x','a/"
      "b','a\\\\b',string.rep('a',33),'a'..string.char(0)}) do "
      "for _,op in ipairs({k.get,k.exists,k.remove}) do local v,e=op(key); "
      "assert(v==nil and e=='kv: invalid key') end;"
      "local v,e=k.set(key,1); assert(v==nil and e=='kv: invalid key') end;"
      "for _,op in ipairs({k.get,k.exists,k.remove}) do assert(not "
      "pcall(op,12)) end;"
      "assert(not pcall(k.set,'x',{})); assert(not pcall(k.set,'x',nil));"
      "assert(not pcall(k.set,'x',function() end));"
      "for _,v in ipairs({math.huge,-math.huge,0/0}) do local "
      "ok,e=k.set('x',v); assert(ok==nil and e=='kv: invalid value') end;"
      "assert(k.set(string.rep('a',32),1)); return 'ok'");
  h2_lua_host_destroy(host);
  for (int configured = 0; configured < 2; ++configured) {
    host = create_host(runtime, configured ? &s_fake_fs_api : NULL);
    expect_ok(host, configured ? NULL : "game",
              "local k=require('kv'); for _,op in "
              "ipairs({k.get,k.set,k.remove,k.exists,k.keys}) do "
              "local v,e=op('x',1); assert(v==nil and e=='kv: unavailable') "
              "end; return 'ok'");
    h2_lua_host_destroy(host);
  }
}

static void test_limits(h2_runtime_t *runtime) {
  fake_reset();
  h2_lua_host_config_t config = host_config(runtime, &s_fake_fs_api);
  config.storage.app_quota_bytes = 64u;
  config.storage.app_max_files = 2u;
  h2_lua_host_t *host = NULL;
  assert(h2_lua_host_create(&config, &host) == H2_PAL_OK);
  assert(h2_lua_host_start(host) == H2_PAL_OK);
  expect_ok(
      host, "game",
      "local k=require('kv'); local s=require('storage');"
      "assert(k.set('x',string.rep('a',30))); "
      "assert(s.get_free_space().used==57);"
      "assert(s.write_file('f','1234567')); assert(s.get_free_space().free==0);"
      "local v,e=s.write_file('f','12345678'); assert(v==nil and e=='storage: "
      "quota exceeded');"
      "v,e=k.set('x',string.rep('a',31)); assert(v==nil and e=='kv: quota "
      "exceeded');"
      "assert(k.set('x',string.rep('b',30)));"
      "v,e=s.write_file('g',''); assert(v==nil and e=='storage: too many "
      "files');"
      "assert(#s.listdir()==1); assert(s.read_file('.kv')==nil);"
      "assert(s.write_file('.kv','x')==nil); assert(s.remove('.kv')==nil);"
      "assert(s.rename('f','.kv')==nil and s.rename('.kv','f')==nil);"
      "assert(k.remove('x')); assert(s.get_free_space().used==7);"
      "assert(s.write_file('g','')); v,e=k.set('x',1); assert(v==nil and "
      "e=='kv: too many files');"
      "assert(s.remove('g')); assert(k.set('x',1)); return 'ok'");
  h2_lua_host_destroy(host);
  fake_reset();
  host = create_host(runtime, &s_fake_fs_api);
  expect_ok(
      host, "game",
      "local k=require('kv'); for i=1,128 do assert(k.set('k'..i,i)) end;"
      "local v,e=k.set('extra',1); assert(v==nil and e=='kv: too many keys');"
      "assert(k.set('k1',false)); assert(#k.keys()==128); return 'ok'");
  h2_lua_host_destroy(host);
}

static void test_faults(h2_runtime_t *runtime) {
  fake_reset();
  h2_lua_host_t *host = create_host(runtime, &s_fake_fs_api);
  expect_ok(host, "game", "assert(require('kv').set('x','old')); return 'ok'");
  int *faults[] = {&s_fs.fail_read_in, &s_fs.fail_write_in, &s_fs.fail_sync_in,
                   &s_fs.fail_close_in, &s_fs.fail_rename_in};
  for (size_t i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i) {
    *faults[i] = 1;
    expect_ok(host, "game",
              "local v,e=require('kv').set('x','new');"
              "assert(v==nil and e=='kv: io error',e); return 'ok'");
    expect_ok(host, "game",
              "assert(require('kv').get('x')=='old'); return 'ok'");
    assert_no_temp_files();
    for (size_t j = 0; j < 4; ++j)
      assert(s_fs.files[j].entry == NULL);
  }
  /* First close completes the old snapshot read; second closes the temp. */
  s_fs.fail_close_in = 2;
  expect_ok(host, "game",
            "local v,e=require('kv').set('x','new');"
            "assert(v==nil and e=='kv: io error'); return 'ok'");
  expect_ok(host, "game", "assert(require('kv').get('x')=='old'); return 'ok'");
  assert_no_temp_files();
  s_fs.fail_remove_in = 1;
  expect_ok(host, "game",
            "local v,e=require('kv').remove('x');"
            "assert(v==nil and e=='kv: io error'); return 'ok'");
  expect_ok(host, "game", "assert(require('kv').get('x')=='old'); return 'ok'");
  h2_lua_host_destroy(host);
}

static uint32_t fixture_crc(const uint8_t *p, size_t n) {
  uint32_t crc = UINT32_MAX;
  for (size_t i = 0; i < n; ++i) {
    crc ^= p[i];
    for (int j = 0; j < 8; ++j)
      crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
  }
  return ~crc;
}
static void fixture_seal(fake_fs_entry_t *e) {
  uint32_t crc = fixture_crc(e->data, e->size - 4);
  for (size_t i = 0; i < 4; ++i)
    e->data[e->size - 4 + i] = (uint8_t)(crc >> (8 * i));
}
static void test_format(h2_runtime_t *runtime) {
  /* Independent fixture generated from the documented bytes using zlib CRC32.
   */
  static const uint8_t golden[] = {
      0x47, 0x5a, 0x4b, 0x56, 1, 0, 0, 0, 1,   0, 0,    0,    8,    0,
      0,    0,    1,    2,    1, 0, 0, 0, 'x', 1, 0x5e, 0x27, 0x54, 0x93};
  fake_reset();
  h2_lua_host_t *host = create_host(runtime, &s_fake_fs_api);
  expect_ok(host, "game", "assert(require('kv').set('x',true)); return 'ok'");
  fake_fs_entry_t *e = fake_find("/data/lua/game/.kv");
  assert(e && e->size == sizeof(golden));
  assert(memcmp(e->data, golden, sizeof(golden)) == 0);
  assert(fixture_crc((const uint8_t *)"123456789", 9) == 0xcbf43926u);
  uint8_t good[sizeof(golden)];
  memcpy(good, e->data, sizeof(good));
  for (size_t i = 0; i < sizeof(good); ++i) {
    memcpy(e->data, good, sizeof(good));
    e->data[i] ^= 0x80;
    expect_ok(host, "game",
              "local k=require('kv'); local v,e=k.get('x');"
              "assert(v==nil and (e=='kv: corrupt data' or e=='kv: unsupported "
              "version'));"
              "assert(k.set('x',false)==nil); assert(k.remove('x')==nil); "
              "return 'ok'");
  }
  /* A well-formed future-version snapshot must not be mistaken for corrupt
   * v1 data, an empty store, or a value which mutations may overwrite. */
  memcpy(e->data, good, sizeof(good));
  e->data[4] = 2;
  fixture_seal(e);
  uint8_t future[sizeof(good)];
  memcpy(future, e->data, sizeof(future));
  expect_ok(
      host, "game",
      "local k=require('kv');"
      "for _,op in ipairs({k.get,k.exists,k.keys,k.remove}) do "
      "local v,e=op('x');assert(v==nil and e=='kv: unsupported version',e) end;"
      "local v,e=k.set('x',false);"
      "assert(v==nil and e=='kv: unsupported version',e);return 'ok'");
  e = fake_find("/data/lua/game/.kv");
  assert(e != NULL && e->size == sizeof(future) &&
         memcmp(e->data, future, sizeof(future)) == 0);
  for (size_t n = 0; n < sizeof(good); ++n) {
    memcpy(e->data, good, sizeof(good));
    e->size = n;
    expect_ok(host, "game",
              "local v,e=require('kv').get('x'); assert(v==nil and e=='kv: "
              "corrupt data'); return 'ok'");
  }
  e->size = sizeof(good);
  memcpy(e->data, good, sizeof(good));
  e->data[23] = 2;
  fixture_seal(e);
  expect_ok(host, "game",
            "local v,e=require('kv').get('x'); assert(v==nil and e=='kv: "
            "corrupt data'); return 'ok'");
  memcpy(e->data, good, sizeof(good));
  expect_ok(host, "game", "assert(require('kv').get('x')==true); return 'ok'");
  h2_lua_host_destroy(host);
}

static void test_retry_and_memory(h2_runtime_t *runtime) {
  fake_reset();
  h2_lua_host_t *host = create_host(runtime, &s_fake_fs_api);
  expect_ok(host, "game", "assert(require('kv').set('x',true)); return 'ok'");
  fake_fs_entry_t *e = fake_find("/data/lua/game/.kv");
  size_t size = e->size;
  s_fs.grow_stat_calls = 8;
  expect_ok(host, "game",
            "local v,e=require('kv').set('x',false);"
            "assert(v==nil and e=='kv: busy',e); return 'ok'");
  assert(s_fs.grow_stat_calls == 0);
  e->size = size;
  expect_ok(host, "game", "assert(require('kv').get('x')==true); return 'ok'");
  s_fs.fail_no_space_in = 1;
  expect_ok(host, "game",
            "local v,e=require('kv').set('x',false);"
            "assert(v==nil and e=='kv: no space',e); return 'ok'");
  expect_ok(
      host, "large",
      "assert(require('kv').set('x',string.rep('a',50000))); return 'ok'");
  h2_lua_host_destroy(host);
  h2_lua_host_config_t config = host_config(runtime, &s_fake_fs_api);
  config.vm_memory_limit_bytes = 100u * 1024u;
  assert(h2_lua_host_create(&config, &host) == H2_PAL_OK);
  assert(h2_lua_host_start(host) == H2_PAL_OK);
  expect_ok(
      host, "large",
      "local k=require('kv'); collectgarbage('stop');"
      "local ok=pcall(k.set,'x',false); assert(not ok);"
      "collectgarbage('restart'); collectgarbage('collect'); return 'ok'");
  expect_ok(
      host, "game",
      "assert(require('kv').set('x',false));"
      "assert(require('storage').write_file('after_oom','ok')); return 'ok'");
  h2_lua_host_destroy(host);
  host = create_host(runtime, &s_fake_fs_api);
  expect_ok(
      host, "large",
      "assert(require('kv').get('x')==string.rep('a',50000)); return 'ok'");
  h2_lua_host_destroy(host);
  assert_no_temp_files();
}

static void run_pair(h2_lua_host_t *host, const char *a, const char *b) {
  h2_lua_job_id_t ids[2];
  const char *scripts[2] = {a, b};
  int done[2] = {0};
  for (size_t i = 0; i < 2; ++i)
    assert(h2_lua_job_submit_text(
               host, "game", "@concurrent.lua", (const uint8_t *)scripts[i],
               strlen(scripts[i]), NULL, 0, &ids[i]) == H2_PAL_OK);
  for (size_t n = 0; n < 10000 && (!done[0] || !done[1]); ++n) {
    assert(h2_lua_host_step(host) == H2_PAL_OK);
    for (size_t i = 0; i < 2; ++i) {
      h2_lua_job_status_t status;
      if (done[i])
        continue;
      assert(h2_lua_job_get_status(host, ids[i], &status) == H2_PAL_OK);
      if (status.state == H2_LUA_JOB_SUCCEEDED) {
        assert(strcmp(status.message, "ok") == 0);
        assert(h2_lua_job_release(host, ids[i]) == H2_PAL_OK);
        done[i] = 1;
      } else {
        if (status.state != H2_LUA_JOB_QUEUED &&
            status.state != H2_LUA_JOB_RUNNING &&
            status.state != H2_LUA_JOB_WAITING) {
          fprintf(stderr, "concurrent failure: %s\n", status.message);
          assert(0);
        }
      }
    }
    (void)h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 1);
  }
  assert(done[0] && done[1]);
}

static void test_concurrency(h2_runtime_t *runtime) {
  fake_reset();
  h2_lua_host_config_t config = host_config(runtime, &s_fake_fs_api);
  h2_lua_host_t *host = NULL;
  config.worker_count = 2;
  assert(h2_lua_host_create(&config, &host) == H2_PAL_OK);
  assert(h2_lua_host_start(host) == H2_PAL_OK);
  run_pair(host,
           "local k=require('kv'); for i=1,40 do local ok,e=k.set('a'..i,i);"
           "while not ok and e=='kv: busy' do ok,e=k.set('a'..i,i) end; "
           "assert(ok,e) end; return 'ok'",
           "local k=require('kv'); for i=1,40 do local ok,e=k.set('b'..i,i);"
           "while not ok and e=='kv: busy' do ok,e=k.set('b'..i,i) end; "
           "assert(ok,e) end; return 'ok'");
  expect_ok(host, "game",
            "local k=require('kv'); assert(#k.keys()==80);"
            "for i=1,40 do assert(k.get('a'..i)==i and k.get('b'..i)==i) end; "
            "return 'ok'");
  run_pair(host,
           "local k=require('kv'); for i=1,40 do assert(k.remove('a'..i)) end; "
           "return 'ok'",
           "local k=require('kv'); for i=1,40 do assert(k.set('b'..i,false)) "
           "end; return 'ok'");
  expect_ok(host, "game",
            "local k=require('kv'); assert(#k.keys()==40);"
            "for i=1,40 do assert(k.get('a'..i)==nil and k.get('b'..i)==false) "
            "end; return 'ok'");
  h2_lua_host_destroy(host);
  fake_reset();
  config.storage.app_quota_bytes = 64;
  config.storage.app_max_files = 2;
  assert(h2_lua_host_create(&config, &host) == H2_PAL_OK);
  assert(h2_lua_host_start(host) == H2_PAL_OK);
  run_pair(host,
           "local v,e=require('kv').set('x',string.rep('a',20)); assert(v or "
           "e=='kv: quota exceeded'); return 'ok'",
           "local v,e=require('storage').write_file('f',string.rep('b',40)); "
           "assert(v or e=='storage: quota exceeded'); return 'ok'");
  uint64_t total = 0;
  fake_fs_entry_t *kv = fake_find("/data/lua/game/.kv"),
                  *f = fake_find("/data/lua/game/f");
  if (kv)
    total += kv->size;
  if (f)
    total += f->size;
  assert(total <= 64 && total >= 40);
  h2_lua_host_destroy(host);
}

static void expect_corrupt(h2_lua_host_t *host) {
  expect_ok(host, "game",
            "local k=require('kv');"
            "for _,op in ipairs({k.get,k.exists,k.keys,k.remove}) do local "
            "v,e=op('a');"
            "assert(v==nil and e=='kv: corrupt data',e) end;"
            "local v,e=k.set('a',true);assert(v==nil and e=='kv: corrupt "
            "data');return 'ok'");
}

static void test_semantic_corruption(h2_runtime_t *runtime) {
  fake_reset();
  h2_lua_host_t *host = create_host(runtime, &s_fake_fs_api);
  expect_ok(host, "game",
            "local "
            "k=require('kv');assert(k.set('a',true));assert(k.set('b',false));"
            "return 'ok'");
  fake_fs_entry_t *e = fake_find("/data/lua/game/.kv");
  uint8_t good[36];
  assert(e->size == sizeof(good));
  memcpy(good, e->data, sizeof(good));
  const struct {
    size_t offset;
    uint8_t value;
  } cases[] = {
      {6, 1},    /* reserved flags */ {8, 129},       /* key count */
      {30, 'a'}, /* duplicate */ {22, 'z'},           /* unsorted */
      {22, 0},   /* NUL key */ {17, 5},               /* unknown type */
      {18, 255}, /* impossible length */ {23, 2},     /* invalid boolean */
      {16, 33},  /* impossible key length */ {12, 15} /* payload mismatch */
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    memcpy(e->data, good, sizeof(good));
    e->data[cases[i].offset] = cases[i].value;
    fixture_seal(e);
    expect_corrupt(host);
  }
  memcpy(e->data, good, sizeof(good));
  /* Valid checksum but one extra payload byte, not part of a record. */
  e->size = 37;
  e->data[12] = 17;
  e->data[32] = 0;
  fixture_seal(e);
  expect_corrupt(host);
  e->size = 36;
  memcpy(e->data, good, sizeof(good));
  expect_ok(host, "game",
            "local "
            "k=require('kv');assert(k.remove('b'));assert(k.set('a',1.0));"
            "return 'ok'");
  e = fake_find("/data/lua/game/.kv");
  /* Replace binary64 with +Inf while maintaining a correct checksum. */
  assert(e->size == 35);
  memset(e->data + 23, 0, 8);
  e->data[29] = 0xf0;
  e->data[30] = 0x7f;
  fixture_seal(e);
  expect_corrupt(host);
  h2_lua_host_destroy(host);
}

static void test_numeric_bytes(h2_runtime_t *runtime) {
  const struct {
    const char *value;
    uint8_t bytes[8];
  } cases[] = {
      {"math.maxinteger", {255, 255, 255, 255, 255, 255, 255, 127}},
      {"math.mininteger", {0, 0, 0, 0, 0, 0, 0, 128}},
      {"1.0", {0, 0, 0, 0, 0, 0, 240, 63}},
      {"-0.0", {0, 0, 0, 0, 0, 0, 0, 128}},
  };
  fake_reset();
  h2_lua_host_t *host = create_host(runtime, &s_fake_fs_api);
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    char script[128];
    snprintf(script, sizeof(script),
             "assert(require('kv').set('x',%s));return 'ok'", cases[i].value);
    expect_ok(host, "game", script);
    fake_fs_entry_t *e = fake_find("/data/lua/game/.kv");
    assert(e && e->size == 35 && e->data[17] == (i < 2 ? 3 : 4));
    assert(memcmp(e->data + 23, cases[i].bytes, 8) == 0);
  }
  h2_lua_host_destroy(host);
}

int main(void) {
  h2_runtime_t *runtime = create_runtime();
  test_roundtrip(runtime);
  test_arguments(runtime);
  test_limits(runtime);
  test_faults(runtime);
  test_format(runtime);
  test_retry_and_memory(runtime);
  test_concurrency(runtime);
  test_semantic_corruption(runtime);
  test_numeric_bytes(runtime);
  h2_runtime_deinit(runtime);
  return 0;
}
