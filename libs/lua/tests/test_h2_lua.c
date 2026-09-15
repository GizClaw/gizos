#include "../src/modules/h2_lua_display_internal.h"
#include "h2_desktop_platform.h"
#include "h2_lua.h"
#include "h2_lua_capability.h"
#include "h2_lua_display.h"
#include "h2_lua_module.h"
#include "h2_lua_esp_claw.h"
#include "h2_lua_event.h"
#include "h2_lua_job.h"
#include "h2_pal.h"

#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "lua.h"
#include "lauxlib.h"

typedef struct test_fs_file {
  const uint8_t *source;
  size_t source_size;
  size_t offset;
} test_fs_file_t;

typedef struct test_fs_entry {
  const char *path;
  const uint8_t *source;
  size_t source_size;
  uint64_t reported_size;
} test_fs_entry_t;

static const uint8_t s_file_main[] =
    "local helper=require('helper');return helper..':'..args.value";
static const uint8_t s_file_helper[] = "return 'file'";
static const uint8_t s_file_bytecode[] = {0x1bu, 'L', 'u', 'a'};
static const uint8_t s_file_malformed[] = "return function(";
static const test_fs_entry_t s_fs_entries[] = {
    {"scripts/main.lua", s_file_main, sizeof(s_file_main) - 1u, 0u},
    {"scripts/helper.lua", s_file_helper, sizeof(s_file_helper) - 1u, 0u},
    {"scripts/bytecode.lua", s_file_bytecode, sizeof(s_file_bytecode), 0u},
    {"scripts/malformed.lua", s_file_malformed, sizeof(s_file_malformed) - 1u,
     0u},
    {"scripts/oversize.lua", NULL, 4097u, 0u},
    {"scripts/invalid_size.lua", NULL, 0u, UINT64_MAX},
};

static const test_fs_entry_t *test_fs_find(const char *path) {
  for (size_t i = 0u; i < sizeof(s_fs_entries) / sizeof(s_fs_entries[0]); ++i) {
    if (strcmp(s_fs_entries[i].path, path) == 0) {
      return &s_fs_entries[i];
    }
  }
  return NULL;
}

static int test_fs_open(void *user, const char *path,
                        h2_pal_fs_open_mode_t mode,
                        h2_pal_fs_file_t **out_file) {
  test_fs_file_t *file = user;
  const test_fs_entry_t *entry = test_fs_find(path);
  if (file == NULL || out_file == NULL || mode != H2_PAL_FS_OPEN_READ) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (entry == NULL) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  file->source = entry->source;
  file->source_size = entry->source_size;
  file->offset = 0u;
  *out_file = (h2_pal_fs_file_t *)file;
  return H2_PAL_OK;
}

static int test_fs_read(void *user, h2_pal_fs_file_t *file_handle, void *data,
                        size_t length, size_t *out_read) {
  test_fs_file_t *file = (test_fs_file_t *)file_handle;
  size_t remaining;
  size_t copied;
  (void)user;
  if (file == NULL || out_read == NULL || (length != 0u && data == NULL) ||
      file->source == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  remaining = file->source_size - file->offset;
  copied = length < remaining ? length : remaining;
  if (copied != 0u) {
    memcpy(data, file->source + file->offset, copied);
  }
  file->offset += copied;
  *out_read = copied;
  return H2_PAL_OK;
}

static int test_fs_close(void *user, h2_pal_fs_file_t *file) {
  (void)user;
  return file == NULL ? H2_PAL_ERR_INVALID_ARG : H2_PAL_OK;
}

static int test_fs_stat(void *user, const char *path,
                        h2_pal_fs_stat_t *out_stat) {
  const test_fs_entry_t *entry = test_fs_find(path);
  (void)user;
  if (out_stat == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (entry == NULL) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  *out_stat = (h2_pal_fs_stat_t){
      .size = entry->reported_size == 0u ? entry->source_size
                                         : entry->reported_size,
      .is_dir = 0,
  };
  return H2_PAL_OK;
}

static test_fs_file_t s_test_fs_file;
static const h2_pal_fs_vtable_t s_test_fs_vtable = {
    .open = test_fs_open,
    .read = test_fs_read,
    .close = test_fs_close,
    .stat = test_fs_stat,
};
static const h2_pal_fs_api_t s_test_fs = {
    .user = &s_test_fs_file,
    .vtable = &s_test_fs_vtable,
};

typedef struct test_display_fixture {
  uint16_t pixels[240u * 240u];
  h2_display_rect_t draw_rects[4096u];
  int width, height;
  size_t draw_count;
  size_t present_count;
  size_t open_count;
  size_t close_count;
  int fail_info;
  size_t fail_draw;
  int fail_present;
  size_t finalizer_count;
} test_display_fixture_t;

static test_display_fixture_t s_test_display_fixture;

static void test_display_reset(void) {
  memset(&s_test_display_fixture, 0, sizeof(s_test_display_fixture));
  s_test_display_fixture.width = s_test_display_fixture.height = 8;
}

static int test_display_open(void *user) {
  ++s_test_display_fixture.open_count;
  assert(user == &s_test_display_fixture);
  return H2_DISPLAY_OK;
}

static int test_display_get_info(void *user, h2_display_info_t *info) {
  assert(user == &s_test_display_fixture);
  if (s_test_display_fixture.fail_info)
    return H2_DISPLAY_ERR_INVALID_ARG;
  if (info == NULL)
    return H2_DISPLAY_ERR_INVALID_ARG;
  *info = (h2_display_info_t){
      .width = s_test_display_fixture.width,
      .height = s_test_display_fixture.height,
      .native_format = H2_DISPLAY_PIXEL_RGB565,
  };
  return H2_DISPLAY_OK;
}

static int test_display_draw_bitmap(void *user, const h2_display_rect_t *rect,
                                    const void *pixels, size_t stride_bytes,
                                    h2_display_pixel_format_t format) {
  test_display_fixture_t *fixture = user;
  const uint8_t *source = pixels;
  int row;
  assert(fixture != NULL && rect != NULL && pixels != NULL);
  assert(format == H2_DISPLAY_PIXEL_RGB565);
  assert(rect->x >= 0 && rect->y >= 0 && rect->width > 0 && rect->height > 0 &&
         rect->x + rect->width <= fixture->width &&
         rect->y + rect->height <= fixture->height);
  assert(stride_bytes >= (size_t)rect->width * sizeof(uint16_t));
  assert(fixture->draw_count <
         sizeof(fixture->draw_rects) / sizeof(fixture->draw_rects[0]));
  fixture->draw_rects[fixture->draw_count++] = *rect;
  if (fixture->fail_draw == fixture->draw_count) return H2_PAL_ERR_IO;
  for (row = 0; row < rect->height; ++row) {
    memcpy(fixture->pixels + (size_t)(rect->y + row) * fixture->width + (size_t)rect->x,
           source + (size_t)row * stride_bytes,
           (size_t)rect->width * sizeof(uint16_t));
  }
  return H2_DISPLAY_OK;
}

static int test_display_present(void *user) {
  test_display_fixture_t *fixture = user;
  assert(fixture != NULL);
  fixture->present_count++;
  if (fixture->fail_present) {
    fixture->fail_present = 0;
    return H2_PAL_ERR_IO;
  }
  return H2_DISPLAY_OK;
}

static int test_display_close(void *user) {
  ++s_test_display_fixture.close_count;
  assert(user == &s_test_display_fixture);
  return H2_DISPLAY_OK;
}

static const h2_pal_display_vtable_t s_test_display_vtable = {
    .open = test_display_open,
    .get_info = test_display_get_info,
    .draw_bitmap = test_display_draw_bitmap,
    .present = test_display_present,
    .close = test_display_close,
};
static const h2_pal_display_api_t s_test_display = {
    .user = &s_test_display_fixture,
    .vtable = &s_test_display_vtable,
};

static int s_test_touch_event_sent;

static h2_pal_result_t test_touch_open(void *user) {
  (void)user;
  s_test_touch_event_sent = 0;
  return H2_PAL_OK;
}

static h2_pal_result_t test_touch_get_info(void *user,
                                           h2_pal_touch_info_t *info) {
  (void)user;
  if (info == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *info = (h2_pal_touch_info_t){368u, 448u};
  return H2_PAL_OK;
}

static h2_pal_result_t test_touch_poll(void *user,
                                       h2_pal_touch_event_t *event) {
  (void)user;
  if (event == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (s_test_touch_event_sent)
    return H2_PAL_ERR_WOULD_BLOCK;
  s_test_touch_event_sent = 1;
  *event = (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_DOWN, 12, 34};
  return H2_PAL_OK;
}

static h2_pal_result_t test_touch_close(void *user) {
  (void)user;
  return H2_PAL_OK;
}

static const h2_pal_touch_vtable_t s_test_touch_vtable = {
    .open = test_touch_open,
    .get_info = test_touch_get_info,
    .poll_event = test_touch_poll,
    .close = test_touch_close,
};

static const h2_pal_touch_api_t s_test_touch = {
    .vtable = &s_test_touch_vtable,
};

/* Records the PCM the module hands to the device so the test can assert that
 * consecutive writes form one gapless stream. */
static uint8_t s_test_audio_written[64];
static size_t s_test_audio_written_bytes;
static size_t s_test_audio_frame_count;

static int test_audio_track_write(h2_pal_audio_track_t *track,
                                  const h2_audio_frame_t *frame,
                                  uint32_t timeout_ms) {
  (void)track;
  (void)timeout_ms;
  if (frame == NULL || frame->bytes != 4u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (s_test_audio_written_bytes + frame->bytes <=
      sizeof(s_test_audio_written)) {
    memcpy(s_test_audio_written + s_test_audio_written_bytes, frame->data,
           frame->bytes);
    s_test_audio_written_bytes += frame->bytes;
  }
  s_test_audio_frame_count++;
  return H2_PAL_OK;
}

static atomic_int s_test_audio_close_count;
static atomic_int s_test_audio_start_count;
static atomic_int s_test_audio_stop_count;
static atomic_int s_test_audio_mic_start_count;
static atomic_int s_test_audio_mic_stop_count;
static atomic_int s_test_audio_mic_block;

static int test_audio_track_close(h2_pal_audio_track_t *track) {
  (void)track;
  (void)atomic_fetch_add(&s_test_audio_close_count, 1);
  return H2_PAL_OK;
}

static h2_pal_audio_track_t s_test_audio_track = {
    .write = test_audio_track_write,
    .close = test_audio_track_close,
};

static int test_audio_start_speaker(void *user) {
  (void)user;
  (void)atomic_fetch_add(&s_test_audio_start_count, 1);
  return H2_PAL_OK;
}

static int test_audio_stop_speaker(void *user) {
  (void)user;
  (void)atomic_fetch_add(&s_test_audio_stop_count, 1);
  return H2_PAL_OK;
}

static int test_audio_start_mic(void *user) {
  (void)user;
  (void)atomic_fetch_add(&s_test_audio_mic_start_count, 1);
  return H2_PAL_OK;
}

static int test_audio_stop_mic(void *user) {
  (void)user;
  (void)atomic_fetch_add(&s_test_audio_mic_stop_count, 1);
  return H2_PAL_OK;
}

static int test_audio_mic_read(void *user, h2_audio_frame_t *frame,
                               uint32_t timeout_ms) {
  static const uint8_t samples[] = {0u, 64u, 0u, 192u};
  (void)user;
  if (frame == NULL || frame->capacity < sizeof(samples)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (atomic_load(&s_test_audio_mic_block) != 0) {
    /* Simulates a microphone that never produces a frame, so callers polling
     * with a long or unbounded timeout stay blocked here until cancelled. */
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  (void)timeout_ms;
  memcpy(frame->data, samples, sizeof(samples));
  frame->bytes = sizeof(samples);
  frame->samples_per_channel = 2u;
  return H2_PAL_OK;
}

static int test_audio_create_track(void *user,
                                   const h2_audio_track_config_t *config,
                                   h2_pal_audio_track_t **out_track) {
  (void)user;
  if (config == NULL || out_track == NULL ||
      config->format.sample_format != H2_AUDIO_SAMPLE_S16LE)
    return H2_PAL_ERR_INVALID_ARG;
  /* Mixer-backed devices reject Tracks whose frame size differs from the
   * playback frame size reported by get_info. */
  if (config->format.frame_samples_per_channel != 2u)
    return H2_PAL_ERR_UNSUPPORTED;
  *out_track = &s_test_audio_track;
  return H2_PAL_OK;
}

static int test_audio_get_info(void *user, h2_audio_info_t *info) {
  (void)user;
  *info = (h2_audio_info_t){
      .available = 1,
      .mic_supported = 1,
      .playback_supported = 1,
      .mic_format =
          {
              .sample_rate_hz = 16000u,
              .frame_samples_per_channel = 2u,
              .channels = 1u,
              .sample_format = H2_AUDIO_SAMPLE_S16LE,
          },
      .playback_format =
          {
              .sample_rate_hz = 16000u,
              .frame_samples_per_channel = 2u,
              .channels = 1u,
              .sample_format = H2_AUDIO_SAMPLE_S16LE,
          },
      .track_queue_frames = 4u,
      .max_tracks = 4u,
  };
  return H2_PAL_OK;
}

static const h2_pal_audio_vtable_t s_test_audio_vtable = {
    .get_info = test_audio_get_info,
    .start_mic = test_audio_start_mic,
    .stop_mic = test_audio_stop_mic,
    .start_speaker = test_audio_start_speaker,
    .stop_speaker = test_audio_stop_speaker,
    .mic_read = test_audio_mic_read,
    .create_track = test_audio_create_track,
};

static const h2_pal_audio_api_t s_test_audio = {
    .vtable = &s_test_audio_vtable,
};

typedef struct test_clock {
  atomic_uint_fast64_t now_ms;
} test_clock_t;

static h2_pal_result_t test_clock_monotonic_ms(void *user, uint64_t *out_ms) {
  test_clock_t *clock = user;
  if (clock == NULL || out_ms == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *out_ms = atomic_fetch_add(&clock->now_ms, 1u);
  return H2_PAL_OK;
}

static h2_pal_result_t test_clock_monotonic_us(void *user, uint64_t *out_us) {
  test_clock_t *clock = user;
  if (clock == NULL || out_us == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *out_us = atomic_load(&clock->now_ms) * 1000u;
  return H2_PAL_OK;
}

static h2_pal_result_t test_clock_sleep_ms(void *user, uint32_t duration_ms) {
  test_clock_t *clock = user;
  if (clock == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  (void)atomic_fetch_add(&clock->now_ms, duration_ms);
  return H2_PAL_OK;
}

static const h2_pal_time_vtable_t s_test_clock_vtable = {
    .get_monotonic_ms = test_clock_monotonic_ms,
    .get_monotonic_us = test_clock_monotonic_us,
    .sleep_ms = test_clock_sleep_ms,
};

static h2_pal_result_t map_list(void *user, h2_runtime_component_t filter,
                                h2_runtime_component_mapping_cb_t callback,
                                void *callback_user) {
  static const struct {
    h2_runtime_component_t component;
    h2_runtime_component_mapping_entry_t entry;
  } mappings[] = {
      {H2_RUNTIME_COMPONENT_BUTTON, {7u, 7u}},
      {H2_RUNTIME_COMPONENT_NFC_READER, {8u, 8u}},
      {H2_RUNTIME_COMPONENT_IMU, {9u, 9u}},
      {H2_RUNTIME_COMPONENT_BUZZER, {10u, 10u}},
  };
  (void)user;
  if (callback == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  for (size_t i = 0u; i < sizeof(mappings) / sizeof(mappings[0]); ++i) {
    if (filter == mappings[i].component) {
      return callback(callback_user, &mappings[i].entry);
    }
  }
  return H2_PAL_OK;
}

static h2_pal_result_t map_get(void *user,
                               h2_runtime_component_id_t component_id,
                               h2_pal_periph_id_t *out_periph_id) {
  (void)user;
  if (out_periph_id == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (component_id < 7u || component_id > 10u)
    return H2_PAL_ERR_NOT_FOUND;
  *out_periph_id = component_id;
  return H2_PAL_OK;
}

static h2_pal_result_t periph_list(void *user, h2_pal_periph_type_t filter,
                                   h2_pal_periph_cb_t callback,
                                   void *callback_user) {
  static const h2_pal_periph_info_t peripherals[] = {
      {.id = 7u, .type = H2_PAL_PERIPH_TYPE_SINGLE_BUTTON, .name = "button"},
      {.id = 8u, .type = H2_PAL_PERIPH_TYPE_NFC_READER, .name = "nfc"},
      {.id = 9u, .type = H2_PAL_PERIPH_TYPE_IMU, .name = "imu"},
      {.id = 10u, .type = H2_PAL_PERIPH_TYPE_BUZZER, .name = "buzzer"},
  };
  (void)user;
  if (callback == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  for (size_t i = 0u; i < sizeof(peripherals) / sizeof(peripherals[0]); ++i) {
    if (filter == H2_PAL_PERIPH_TYPE_ANY || filter == peripherals[i].type) {
      h2_pal_result_t result = callback(callback_user, &peripherals[i]);
      if (result != H2_PAL_OK)
        return result;
    }
  }
  return H2_PAL_OK;
}

static h2_pal_result_t periph_get(void *user, h2_pal_periph_id_t id,
                                  h2_pal_periph_info_t *out_info) {
  static const h2_pal_periph_type_t types[] = {
      H2_PAL_PERIPH_TYPE_SINGLE_BUTTON,
      H2_PAL_PERIPH_TYPE_NFC_READER,
      H2_PAL_PERIPH_TYPE_IMU,
      H2_PAL_PERIPH_TYPE_BUZZER,
  };
  (void)user;
  if (out_info == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (id < 7u || id > 10u)
    return H2_PAL_ERR_NOT_FOUND;
  *out_info = (h2_pal_periph_info_t){
      .id = id,
      .type = types[id - 7u],
      .name = "lua-test",
  };
  return H2_PAL_OK;
}

static h2_runtime_t *create_runtime_with_time(const h2_pal_time_api_t *time) {
  static const h2_runtime_component_mapper_vtable_t mapper_vtable = {
      .list = map_list,
      .get_periph_id = map_get,
  };
  static const h2_runtime_component_mapper_t mapper = {
      .vtable = &mapper_vtable,
  };
  static const h2_pal_periph_vtable_t periph_vtable = {
      .list = periph_list,
      .get = periph_get,
  };
  static const h2_pal_periph_api_t periph = {
      .vtable = &periph_vtable,
  };
  h2_runtime_config_t config = {
      .board = "test",
      .target = "desktop",
      .chip = "host",
      .firmware_info = h2_pal_unsupported_firmware_info_api(),
      .mem = h2_desktop_platform_default_allocator(),
      .log = h2_desktop_platform_log_api(),
      .time = time,
      .timer = h2_pal_unsupported_timer_api(),
      .task = h2_desktop_platform_task_api(),
      .queue = h2_desktop_platform_queue_api(),
      .sync = h2_desktop_platform_sync_api(),
      .fs = &s_test_fs,
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
      .display = &s_test_display,
      .audio = &s_test_audio,
      .audio_decoder = h2_pal_unsupported_audio_decoder_api(),
      .periph = &periph,
      .button = h2_desktop_platform_button_api(),
      .touch = &s_test_touch,
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
      .component_mapper = &mapper,
  };
  h2_runtime_t *runtime = NULL;
  assert(h2_runtime_init(&config, &runtime) == H2_PAL_OK);
  assert(h2_runtime_input_start(runtime, NULL) == H2_PAL_OK);
  return runtime;
}

static h2_runtime_t *create_runtime(void) {
  return create_runtime_with_time(h2_desktop_platform_time_api());
}

static h2_lua_host_t *create_unstarted_host_with_scheduler(
    h2_runtime_t *runtime, uint32_t instruction_quantum,
    uint32_t resume_time_budget_ms, uint32_t execution_timeout_ms,
    size_t source_limit_bytes) {
  h2_lua_host_t *host = NULL;
  h2_lua_host_config_t config;
  config = (h2_lua_host_config_t){
      .runtime = runtime,
      .worker_count = 1u,
      .max_jobs = 4u,
      .event_delivery_capacity = 16u,
      .callback_capacity_per_job = 8u,
      .pending_capability_capacity = 1u,
      .vm_memory_limit_bytes = 256u * 1024u,
      .source_limit_bytes = source_limit_bytes,
      .output_limit_bytes = 128u,
      .instruction_quantum = instruction_quantum,
      .resume_time_budget_ms = resume_time_budget_ms,
      .execution_timeout_ms = execution_timeout_ms,
  };
  assert(h2_lua_host_create(&config, &host) == H2_PAL_OK);
  return host;
}

static h2_lua_host_t *create_unstarted_host(h2_runtime_t *runtime) {
  return create_unstarted_host_with_scheduler(runtime, 1000u, 0u, 1000u, 4096u);
}

static h2_lua_host_t *create_host(h2_runtime_t *runtime) {
  h2_lua_host_t *host = create_unstarted_host(runtime);
  assert(h2_lua_host_start(host) == H2_PAL_OK);
  return host;
}

static void
assert_missing_worker_apis_are_rejected(const h2_runtime_t *runtime) {
  /* Probe a copy: the shared runtime's input task reads queue concurrently. */
  h2_runtime_t probe = *runtime;
  h2_pal_queue_vtable_t fallback_vtable = *runtime->queue->vtable;
  h2_pal_queue_api_t fallback_queue = {
      .user = runtime->queue->user,
      .vtable = &fallback_vtable,
  };
  h2_lua_host_t *host = NULL;
  h2_lua_host_config_t config = {.runtime = &probe};
  probe.queue = NULL;
  assert(h2_lua_host_create(&config, &host) == H2_PAL_ERR_UNSUPPORTED);
  assert(host == NULL);
  probe.queue = runtime->queue;
  probe.task = NULL;
  assert(h2_lua_host_create(&config, &host) == H2_PAL_ERR_UNSUPPORTED);
  assert(host == NULL);
  probe.task = runtime->task;

  fallback_vtable.send_latest = NULL;
  probe.queue = &fallback_queue;
  assert(h2_lua_host_create(&config, &host) == H2_PAL_OK);
  assert(h2_lua_host_start(host) == H2_PAL_OK);
  assert(h2_lua_host_step(host) == H2_PAL_OK);
  h2_lua_host_destroy(host);
}

typedef struct capability_fixture {
  h2_lua_host_t *host;
  h2_lua_job_id_t job_id;
  h2_lua_capability_request_id_t pending_id;
  size_t cancel_count;
  h2_pal_result_t cancel_status_result;
} capability_fixture_t;

static h2_pal_result_t
immediate_capability(void *user, h2_lua_capability_request_id_t request_id,
                     const char *input, const char *options, char *output,
                     size_t output_capacity, const char **out_error) {
  (void)user;
  (void)request_id;
  (void)out_error;
  assert(strcmp(input, "{\"a\":1}") == 0);
  assert(strcmp(options, "{\"z\":true}") == 0);
  assert(output_capacity >= 3u);
  strcpy(output, "ok");
  return H2_PAL_OK;
}

static h2_pal_result_t
pending_capability(void *user, h2_lua_capability_request_id_t request_id,
                   const char *input, const char *options, char *output,
                   size_t output_capacity, const char **out_error) {
  capability_fixture_t *fixture = user;
  (void)input;
  (void)options;
  (void)output;
  (void)output_capacity;
  (void)out_error;
  fixture->pending_id = request_id;
  return H2_PAL_ERR_WOULD_BLOCK;
}

static h2_pal_result_t completed_before_return_capability(
    void *user, h2_lua_capability_request_id_t request_id, const char *input,
    const char *options, char *output, size_t output_capacity,
    const char **out_error) {
  capability_fixture_t *fixture = user;
  (void)input;
  (void)options;
  (void)output;
  (void)output_capacity;
  (void)out_error;
  assert(h2_lua_capability_complete(fixture->host, request_id, H2_PAL_OK,
                                    "async", NULL) == H2_PAL_OK);
  return H2_PAL_ERR_WOULD_BLOCK;
}

static void cancel_capability(void *user,
                              h2_lua_capability_request_id_t request_id) {
  capability_fixture_t *fixture = user;
  h2_lua_job_status_t job_status;
  assert(request_id == fixture->pending_id);
  fixture->cancel_status_result =
      h2_lua_job_get_status(fixture->host, fixture->job_id, &job_status);
  fixture->cancel_count++;
}

static h2_lua_job_status_t status(h2_lua_host_t *host, h2_lua_job_id_t id) {
  h2_lua_job_status_t value;
  assert(h2_lua_job_get_status(host, id, &value) == H2_PAL_OK);
  return value;
}

static void run_until_terminal(h2_lua_host_t *host, h2_lua_job_id_t id,
                               size_t step_limit) {
  size_t step;
  for (step = 0u; step < step_limit; ++step) {
    h2_lua_job_state_t state = status(host, id).state;
    if (state == H2_LUA_JOB_SUCCEEDED || state == H2_LUA_JOB_FAILED ||
        state == H2_LUA_JOB_CANCELLED || state == H2_LUA_JOB_TIMED_OUT ||
        state == H2_LUA_JOB_STOPPED) {
      return;
    }
    assert(h2_lua_host_step(host) == H2_PAL_OK);
    (void)h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 1u);
  }
  assert(!"Lua job did not reach a terminal state");
}

typedef struct expected_pixel {
  int x;
  int y;
} expected_pixel_t;

static h2_lua_job_status_t run_display_script_size(h2_lua_host_t *host,
                                              const char *name,
                                              const uint8_t *script,
                                              size_t script_size,
                                              int width, int height) {
  h2_lua_job_id_t job_id;
  h2_lua_job_status_t job_status;
  test_display_reset();
  s_test_display_fixture.width = width;
  s_test_display_fixture.height = height;
  assert(h2_lua_job_submit_text(host, NULL, name, script, script_size, NULL, 0u,
                                &job_id) == H2_PAL_OK);
  /* Pixel oracles run on an independent worker with up to a five-second job
   * deadline. Allow that deadline to report failure instead of imposing a
   * 64 ms scheduler-speed requirement on loaded CI hosts. */
  run_until_terminal(host, job_id, 6000u);
  job_status = status(host, job_id);
  if (job_status.state != H2_LUA_JOB_SUCCEEDED)
    fprintf(stderr, "%s: %s\n", name, job_status.message);
  assert(job_status.state == H2_LUA_JOB_SUCCEEDED);
  assert(h2_lua_job_release(host, job_id) == H2_PAL_OK);
  return job_status;
}

static h2_lua_job_status_t run_display_script(h2_lua_host_t *host,
                                              const char *name,
                                              const uint8_t *script,
                                              size_t script_size) {
  return run_display_script_size(host, name, script, script_size, 8, 8);
}

static void assert_draw_rect(size_t index, int x, int y, int width,
                             int height) {
  const h2_display_rect_t *rect;
  assert(index < s_test_display_fixture.draw_count);
  rect = &s_test_display_fixture.draw_rects[index];
  assert(rect->x == x && rect->y == y && rect->width == width &&
         rect->height == height);
}

static void assert_only_pixels(uint16_t color, const expected_pixel_t *expected,
                               size_t expected_count) {
  int x;
  int y;
  for (y = 0; y < 8; ++y) {
    for (x = 0; x < 8; ++x) {
      int found = 0;
      size_t index;
      for (index = 0u; index < expected_count; ++index) {
        if (expected[index].x == x && expected[index].y == y) {
          found = 1;
          break;
        }
      }
      assert(s_test_display_fixture.pixels[(size_t)y * 8u + (size_t)x] ==
             (found ? color : 0u));
    }
  }
}

static void test_borrowed_display(void) {
  h2_runtime_t *runtime = create_runtime();
  const char *scripts[] = {
      "local d=require('display');d.present()",
      "local d=require('display');d.present();d.deinit()",
      "local d=require('display');error('after-open')",
      "require('display')",
      "local d=require('display');require('runtime').sleep(100000)",
  };
  for (size_t i = 0; i < sizeof(scripts) / sizeof(scripts[0]); ++i) {
    h2_lua_host_t *host = NULL;
    const h2_lua_host_config_t config = {
        .runtime = runtime, .worker_count = 1u, .max_jobs = 1u,
        .borrow_display = 1, .execution_timeout_ms = 200000u,
    };
    test_display_reset();
    assert(h2_pal_display_open(runtime->display) == H2_PAL_OK);
    s_test_display_fixture.fail_info = i == 3u;
    assert(h2_lua_host_create(&config, &host) == H2_PAL_OK);
    assert(h2_lua_host_start(host) == H2_PAL_OK);
    h2_lua_job_id_t job;
    assert(h2_lua_job_submit_text(
               host, NULL, "@borrowed-display.lua", (const uint8_t *)scripts[i],
               strlen(scripts[i]), NULL, 0u, &job) == H2_PAL_OK);
    if (i == 4u) {
      for (unsigned wait = 0u; wait < 1000u &&
           status(host, job).state != H2_LUA_JOB_WAITING; ++wait)
        assert(h2_pal_time_sleep_ms(runtime->time, 1u) == H2_PAL_OK);
      assert(status(host, job).state == H2_LUA_JOB_WAITING);
      assert(h2_lua_host_stop(host) == H2_PAL_OK);
      assert(h2_lua_host_join(host) == H2_PAL_OK);
    } else {
      run_until_terminal(host, job, 64u);
      assert(status(host, job).state ==
          (i >= 2u ? H2_LUA_JOB_FAILED : H2_LUA_JOB_SUCCEEDED));
      assert(h2_lua_job_release(host, job) == H2_PAL_OK);
    }
    h2_lua_host_destroy(host);
    assert(s_test_display_fixture.open_count == 1u);
    assert(s_test_display_fixture.close_count == 0u);
    assert(h2_pal_display_close(runtime->display) == H2_PAL_OK);
    assert(s_test_display_fixture.close_count == 1u);
  }
  h2_runtime_deinit(runtime);
}

typedef struct mesh_allocator_probe {
  lua_Alloc allocate;
  void *user;
  size_t calls;
} mesh_allocator_probe_t;

static void *mesh_probe_allocate(void *user, void *ptr, size_t old_size,
                                 size_t new_size) {
  mesh_allocator_probe_t *probe = user;
  ++probe->calls;
  return probe->allocate(probe->user, ptr, old_size, new_size);
}

static int test_mesh_new(lua_State *state) {
  h2_lua_display_vertex_t vertices[] = {{1,1},{4,1},{4,4},{1,4}};
  h2_lua_display_primitive_t primitive = {H2_LUA_DISPLAY_POLYGON,0,4,0xf800};
  h2_lua_display_mesh_config_t config = {6,2,{vertices,4,&primitive,1}};
  int top = lua_gettop(state);
  assert(h2_lua_display_mesh_push(NULL, &config) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_lua_display_mesh_push(state, NULL) == H2_PAL_ERR_INVALID_ARG);
  config.vertex_capacity = H2_LUA_DISPLAY_VERTEX_LIMIT + 1u;
  assert(h2_lua_display_mesh_push(state, &config) == H2_PAL_ERR_INVALID_ARG);
  config = (h2_lua_display_mesh_config_t){H2_LUA_DISPLAY_VERTEX_LIMIT,0,{0}};
  assert(h2_lua_display_mesh_push(state, &config) == H2_PAL_ERR_NO_MEMORY);
  assert(lua_gettop(state) == top);
  config = (h2_lua_display_mesh_config_t){0};
  assert(h2_lua_display_mesh_push(state, &config) == H2_PAL_OK);
  lua_pop(state, 1);
  config = (h2_lua_display_mesh_config_t){6,2,{vertices,4,&primitive,1}};
  assert(h2_lua_display_mesh_push(state, &config) == H2_PAL_OK);
  assert(lua_gettop(state) == top + 1);
  vertices[0].x = 1000; /* Public push copied stack-owned data. */
  primitive.color = 0;
  return 1;
}

static int test_mesh_update(lua_State *state) {
  int mode = (int)luaL_checkinteger(state, 2);
  assert(lua_checkstack(state, 4));
  if (mode == 0) (void)lua_newuserdatauv(state, 8u, 0);
  int top = lua_gettop(state);
  h2_lua_display_vertex_t vertices[] = {{1,6},{6,6}};
  h2_lua_display_primitive_t primitive = {H2_LUA_DISPLAY_LINE,0,2,0x001f};
  h2_lua_display_mesh_data_t data = {vertices,2,&primitive,1};
  mesh_allocator_probe_t probe = {0};
  probe.allocate = lua_getallocf(state, &probe.user);
  lua_setallocf(state, mesh_probe_allocate, &probe);
  if (mode == 0) {
    assert(h2_lua_display_mesh_update(state, -1, &data) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_lua_display_mesh_update(NULL, 1, &data) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_lua_display_mesh_update(state, 1, NULL) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_lua_display_mesh_update(state, 0, &data) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_lua_display_mesh_update(state, 2, &data) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_lua_display_mesh_update(state, 99, &data) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_lua_display_mesh_update(state, LUA_REGISTRYINDEX, &data) ==
           H2_PAL_ERR_INVALID_ARG);
    vertices[1].x = NAN;
    assert(h2_lua_display_mesh_update(state, 1, &data) == H2_PAL_ERR_INVALID_ARG);
    vertices[1].x = 6;
    primitive.first = SIZE_MAX;
    assert(h2_lua_display_mesh_update(state, 1, &data) == H2_PAL_ERR_INVALID_ARG);
    primitive.first = 0; primitive.count = SIZE_MAX;
    assert(h2_lua_display_mesh_update(state, 1, &data) == H2_PAL_ERR_INVALID_ARG);
    primitive.count = 2; primitive.kind = (h2_lua_display_primitive_kind_t)9;
    assert(h2_lua_display_mesh_update(state, 1, &data) == H2_PAL_ERR_INVALID_ARG);
    data.vertex_count = 7;
    assert(h2_lua_display_mesh_update(state, 1, &data) == H2_PAL_ERR_INVALID_ARG);
  } else if (mode == 1) {
    assert(h2_lua_display_mesh_update(state, -2, &data) == H2_PAL_OK);
  } else {
    data = (h2_lua_display_mesh_data_t){0};
    assert(h2_lua_display_mesh_update(state, 1, &data) == H2_PAL_OK);
  }
  lua_setallocf(state, probe.allocate, probe.user);
  assert(probe.calls == 0u);
  assert(lua_gettop(state) == top);
  return 0;
}

static int test_mesh_open(void *lua_state, void *user) {
  lua_State *state = lua_state;
  (void)user;
  lua_newtable(state);
  lua_pushcfunction(state, test_mesh_new); lua_setfield(state, -2, "new");
  lua_pushcfunction(state, test_mesh_update); lua_setfield(state, -2, "update");
  return 1;
}

static int test_raster_noalloc(lua_State *state) {
  luaL_checktype(state, 1, LUA_TFUNCTION);
  assert(lua_checkstack(state, 128));
  lua_pushvalue(state, 1);
  assert(lua_pcall(state, 0, 0, 0) == LUA_OK);
  lua_gc(state, LUA_GCSTOP);
  mesh_allocator_probe_t probe = {0};
  probe.allocate = lua_getallocf(state, &probe.user);
  lua_setallocf(state, mesh_probe_allocate, &probe);
  lua_pushvalue(state, 1);
  int result = lua_pcall(state, 0, 0, 0);
  lua_setallocf(state, probe.allocate, probe.user);
  lua_gc(state, LUA_GCRESTART);
  assert(result == LUA_OK && probe.calls == 0u);
  return 0;
}

static void *test_raster_fail_allocate(void *user, void *ptr, size_t old_size,
                                       size_t new_size) {
  mesh_allocator_probe_t *probe = user;
  if (new_size != 0u && (ptr == NULL || new_size > old_size))
    return NULL;
  return probe->allocate(probe->user, ptr, old_size, new_size);
}

static int test_raster_oom(lua_State *state) {
  luaL_checktype(state, 1, LUA_TFUNCTION);
  assert(lua_checkstack(state, 128));
  mesh_allocator_probe_t probe = {0};
  probe.allocate = lua_getallocf(state, &probe.user);
  lua_pushvalue(state, 1);
  lua_setallocf(state, test_raster_fail_allocate, &probe);
  int result = lua_pcall(state, 0, 1, 0);
  lua_setallocf(state, probe.allocate, probe.user);
  assert(result == LUA_ERRMEM);
  lua_pop(state, 1);
  return 0;
}

typedef struct raster_measure_probe {
  lua_Alloc allocate;
  void *user;
  size_t calls, current, peak;
} raster_measure_probe_t;

static void *raster_measure_allocate(void *user, void *ptr, size_t old_size,
                                     size_t new_size) {
  raster_measure_probe_t *probe = user;
  size_t old = ptr == NULL ? 0 : old_size;
  void *result = probe->allocate(probe->user, ptr, old_size, new_size);
  if (new_size == 0 || result != NULL) {
    probe->current = probe->current - old + new_size;
    if (probe->current > probe->peak)
      probe->peak = probe->current;
  }
  if (new_size != 0)
    ++probe->calls;
  return result;
}

static int test_raster_measure(lua_State *state) {
  const char *name = luaL_checkstring(state, 1);
  luaL_checktype(state, 2, LUA_TFUNCTION);
  lua_Integer boundaries = luaL_checkinteger(state, 3);
  int once = lua_toboolean(state, 4);
  uint64_t samples[32];
  size_t calls = 0, peak = 0;
  assert(lua_checkstack(state, 128));
  unsigned count = once ? 1u : 32u;
  lua_gc(state, LUA_GCCOLLECT);
  lua_gc(state, LUA_GCSTOP);
  for (unsigned i = 0; i < count + (once ? 0u : 4u); ++i) {
    raster_measure_probe_t probe = {0};
    probe.allocate = lua_getallocf(state, &probe.user);
    probe.current = (size_t)lua_gc(state, LUA_GCCOUNT) * 1024u +
                    (size_t)lua_gc(state, LUA_GCCOUNTB);
    probe.peak = probe.current;
    uint64_t start = 0, end = 0;
    const h2_pal_time_api_t *time = h2_desktop_platform_time_api();
    assert(h2_pal_time_get_monotonic_us(time, &start) == H2_PAL_OK);
    lua_setallocf(state, raster_measure_allocate, &probe);
    lua_pushvalue(state, 2);
    lua_pushinteger(state, i);
    int result = lua_pcall(state, 1, 0, 0);
    lua_setallocf(state, probe.allocate, probe.user);
    assert(h2_pal_time_get_monotonic_us(time, &end) == H2_PAL_OK);
    if (result != LUA_OK)
      fprintf(stderr, "raster benchmark %s: %s\n", name,
              lua_tostring(state, -1));
    assert(result == LUA_OK);
    if (once || i >= 4u) {
      samples[once ? 0 : i - 4u] = end - start;
      calls += probe.calls;
      if (probe.peak > peak)
        peak = probe.peak;
    }
  }
  lua_gc(state, LUA_GCRESTART);
  for (unsigned i = 1; i < count; ++i) {
    uint64_t value = samples[i];
    unsigned j = i;
    while (j > 0 && samples[j - 1] > value) {
      samples[j] = samples[j - 1];
      --j;
    }
    samples[j] = value;
  }
  printf(
      "raster_lua phase=%s samples=%u warmup=%u p50_us=%llu p95_us=%llu "
      "peak_vm_bytes=%zu allocation_calls=%zu boundary_calls_per_sample=%lld\n",
      name, count, once ? 0u : 4u, (unsigned long long)samples[count / 2],
      (unsigned long long)samples[(count * 95u) / 100u], peak, calls,
      (long long)boundaries);
  return 0;
}

static int test_region_failure(lua_State *state);

/* Independent extraction-source oracle; no production transform helper. */
static int test_mesh_source(lua_State *s) {
  double x = luaL_checknumber(s, 2), y = luaL_checknumber(s, 3);
  double scale = luaL_checknumber(s, 4), angle = luaL_checknumber(s, 5);
  double grid = luaL_checknumber(s, 6), ca = cos(angle), sa = sin(angle);
  size_t n = lua_rawlen(s, 1);
  lua_settop(s, 7);
  lua_createtable(s, (int)n, 0);
  for (size_t i = 0; i < n; ++i) {
    lua_rawgeti(s, 1, (lua_Integer)i + 1);
    lua_rawgeti(s, -1, 1); double vx = lua_tonumber(s, -1); lua_pop(s, 1);
    lua_rawgeti(s, -1, 2); double vy = lua_tonumber(s, -1); lua_pop(s, 2);
    float fx=((float)x+((float)vx*(float)ca-(float)vy*(float)sa)*(float)scale)/(float)grid;
    float fy=((float)y+((float)vx*(float)sa+(float)vy*(float)ca)*(float)scale)/(float)grid;
    float error=64*FLT_EPSILON*(fabsf((float)x)+fabsf((float)y)+(fabsf((float)vx)+fabsf((float)vy))*(float)scale+1);
    double px,py;
    if(fabs(vx)<=100000 && fabs(vy)<=100000 && error<.25f && fabsf(fx-floorf(fx)-.5f)>error)
      px=floorf(fx+.5f)*(float)grid;
    else px=floor((x+(vx*ca-vy*sa)*scale)/grid+.5)*grid;
    if(fabs(vx)<=100000 && fabs(vy)<=100000 && error<.25f && fabsf(fy-floorf(fy)-.5f)>error)
      py=floorf(fy+.5f)*(float)grid;
    else py=floor((y+(vx*sa+vy*ca)*scale)/grid+.5)*grid;
    if (!lua_isnoneornil(s, 7)) {
      h2_lua_display_mesh_t *mesh = lua_touserdata(s, 7);
      assert(mesh->positions_valid && mesh->source_transform);
      h2_lua_display_vertex_t *positions =
          (h2_lua_display_vertex_t *)(mesh + 1) + mesh->vertex_capacity;
      assert(positions[i].x == px && positions[i].y == py);
    }
    lua_createtable(s,2,0);
    lua_pushnumber(s,px);lua_rawseti(s,-2,1);
    lua_pushnumber(s,py);lua_rawseti(s,-2,2);
    lua_rawseti(s,-2,(lua_Integer)i+1);
  }
  return 1;
}

/* Poison one private replay record. A reraster overwrites the marker even
 * when pixels/present would otherwise be identical. No production counter. */
static int test_mesh_marker(lua_State *s) {
  assert(lua_getiuservalue(s, 1, 1) == LUA_TUSERDATA);
  display_span_cache_t *cache = lua_touserdata(s, -1);
  assert(cache->valid && cache->count > 0);
  if (lua_toboolean(s, 2)) cache->spans[0].color = 0xabcd;
  lua_pushboolean(s, cache->spans[0].color == 0xabcd);
  return 1;
}

static int test_raster_open(void *lua_state, void *user) {
  lua_State *state = lua_state;
  (void)user;
  lua_newtable(state);
  lua_pushcfunction(state, test_mesh_source);
  lua_setfield(state, -2, "mesh_source");
  lua_pushcfunction(state, test_mesh_marker);
  lua_setfield(state, -2, "mesh_marker");
  lua_pushcfunction(state, test_raster_noalloc);
  lua_setfield(state, -2, "noalloc");
  lua_pushcfunction(state, test_raster_measure);
  lua_setfield(state, -2, "measure");
  lua_pushcfunction(state, test_region_failure);
  lua_setfield(state, -2, "fail");
  lua_pushcfunction(state, test_raster_oom);
  lua_setfield(state, -2, "oom");
  return 1;
}

static void test_display_raster2d(int benchmark, const char *path) {
  h2_runtime_t *runtime = create_runtime();
  h2_lua_host_t *host = NULL;
  h2_lua_host_config_t config = {
      .runtime = runtime,
      .worker_count = 1,
      .max_jobs = 1,
      .instruction_quantum = 1000000000,
      .execution_timeout_ms = 20000,
      .source_limit_bytes = 16384,
      .vm_memory_limit_bytes = benchmark ? 8u * 1024u * 1024u : 256u * 1024u};
  assert(h2_lua_host_create(&config, &host) == H2_PAL_OK);
  assert(h2_lua_register_module(host, "raster_test", test_raster_open, NULL) ==
         H2_PAL_OK);
  assert(h2_lua_host_start(host) == H2_PAL_OK);
  FILE *file = fopen(path, "rb");
  assert(file != NULL);
  uint8_t script[16384];
  size_t size = fread(script, 1, sizeof(script), file);
  assert(!ferror(file) && size < sizeof(script));
  assert(fclose(file) == 0);
  (void)run_display_script_size(host, path, script, size, benchmark ? 240 : 8,
                                benchmark ? 240 : 8);
  h2_lua_host_destroy(host);
  h2_runtime_deinit(runtime);
}

static void test_display_meshes(void) {
  h2_runtime_t *runtime = create_runtime();
  h2_lua_host_t *host = create_unstarted_host(runtime);
  assert(h2_lua_register_module(host, "kv", test_mesh_open, NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_lua_register_module(host, "vmath", test_mesh_open, NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_lua_register_module(host, "geometry", test_mesh_open, NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_lua_register_module(host, "mesh_test", test_mesh_open, NULL) == H2_PAL_OK);
  assert(h2_lua_host_start(host) == H2_PAL_OK);
  static const struct { const char *draw; const char *pixels; } cases[] = {
      {"local m=n.new();n.update(m,0);collectgarbage('collect');d.draw_mesh(m)",
       "........" ".####..." ".####..." ".####..."
       "........" "........" "........" "........"},
      {"local m=n.new();d.draw_mesh(m);d.clear('black');"
       "n.update(m,0);d.draw_mesh(m)",
       "........" ".####..." ".####..." ".####..."
       "........" "........" "........" "........"},
      {"local m=n.new();n.update(m,1);d.draw_mesh(m,{color='red'})",
       "........" "........" "........" "........"
       "........" "........" ".######." "........"},
      {"local m=n.new();d.draw_mesh(m);d.clear('black');"
       "n.update(m,2);d.draw_mesh(m)",
       "........" "........" "........" "........"
       "........" "........" "........" "........"},
      {"local m=n.new();d.draw_mesh(m);d.clear('black');"
       "n.update(m,1);d.draw_mesh(m,{color='red',left=2,right=5})",
       "........" "........" "........" "........"
       "........" "........" "..###..." "........"},
      {"local m=n.new();d.draw_mesh(m);d.clear('black');"
       "d.draw_mesh(m,{matrix={1,0,0,1,.5,0},offset_x=.5,left=3,right=5,top=2,bottom=4})",
       "........" "........" "...##..." "...##..."
       "........" "........" "........" "........"},
      {"local m=n.new();d.draw_mesh(m,{grid=2})",
       "........" "........" "..###..." "..###..."
       "........" "........" "........" "........"},
      {"local m=n.new();d.draw_mesh(m,{grid=2});d.clear('black');d.draw_mesh(m)",
       "........" ".####..." ".####..." ".####..."
       "........" "........" "........" "........"},
      {"local m=n.new();d.draw_mesh(m,{matrix={-1,0,0,1,6,0}})",
       "........" "..####.." "..####.." "..####.."
       "........" "........" "........" "........"},
      {"local v={{1,1},{4,1},{4,4},{1,4}};"
       "local p={{0,1,4,'red'}};local m=d.compile_mesh(v,p,6,2);"
       "v[1][1]=1000;p[1][4]='blue';d.draw_mesh(m)",
       "........" ".####..." ".####..." ".####..."
       "........" "........" "........" "........"},
      {"local m=n.new();d.draw_mesh(m);d.clear('black');"
       "d.update_mesh(m,{{1,6},{6,6}},{{1,1,2,'red'}});d.draw_mesh(m)",
       "........" "........" "........" "........"
       "........" "........" ".######." "........"},
      {"local m=d.compile_mesh({}, {},6,2);n.update(m,1);"
       "d.draw_mesh(m,{color='red'})",
       "........" "........" "........" "........"
       "........" "........" ".######." "........"},
      {"local m=n.new();assert(not pcall(d.update_mesh,m,{{1,6},{6,6}},"
       "{{1,1,2,'blue'},{0,1,3,'blue'}}));d.draw_mesh(m)",
       "........" ".####..." ".####..." ".####..."
       "........" "........" "........" "........"},
  };
  for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
    char script[2048];
    int size = snprintf(script, sizeof(script),
        "local d=require('display');local n=require('mesh_test');%s;d.present()",
        cases[i].draw);
    assert(size > 0 && (size_t)size < sizeof(script));
    (void)run_display_script(host, "@mesh-pixels.lua", (const uint8_t *)script,
                             (size_t)size);
    assert(strlen(cases[i].pixels) == 64u);
    for (size_t pixel = 0; pixel < 64u; ++pixel) {
      uint16_t expected = cases[i].pixels[pixel] == '#' ? 0xf800u : 0u;
      if (s_test_display_fixture.pixels[pixel] != expected)
        fprintf(stderr, "mesh case=%zu pixel=%zu actual=%04x expected=%04x\n",
                i, pixel, s_test_display_fixture.pixels[pixel], expected);
      assert(s_test_display_fixture.pixels[pixel] == expected);
    }
  }
  static const uint8_t invalid[] =
      "local d=require('display');local n=require('mesh_test');local m=n.new();"
      "local function bad(f,...) assert(not pcall(f,...)) end;d.present();"
      "bad(d.compile_mesh,{}, {},-1,0);bad(d.compile_mesh,{}, {},65537,0);"
      "bad(d.compile_mesh,{{0/0,0}},{});bad(d.compile_mesh,{},{{1,1,2,'red'}});"
      "bad(d.draw_mesh,{});bad(d.draw_mesh,m,{grid=-1});"
      "bad(d.draw_mesh,m,{grid=17});bad(d.draw_mesh,m,{grid=.5});"
      "bad(d.draw_mesh,m,{right=9});bad(d.draw_mesh,m,{left=4294967296});"
      "bad(d.draw_mesh,m,{matrix={1000000,0,0,1000000,0,0},offset_x=100001});"
      "bad(d.draw_mesh,m,{matrix={0/0,0,0,1,0,0}});"
      "bad(d.draw_mesh,m,{color='unknown'});"
      "d.draw_mesh(m,{left=2,right=2});d.draw_mesh(d.compile_mesh({},{}));"
      "local huge=d.compile_mesh({{1000000,0}},{});"
      "bad(d.draw_mesh,huge,{matrix={1000000,0,0,1,0,0}});d.present();"
      "local weak=setmetatable({m},{__mode='v'});m=nil;collectgarbage('collect');"
      "assert(weak[1]==nil);m=n.new();"
      "local color=setmetatable({},{__index=function()d.deinit();return 255 end});"
      "bad(d.draw_mesh,m,{color=color});bad(d.draw_mesh,m)";
  (void)run_display_script(host, "@mesh-invalid.lua", invalid, sizeof(invalid)-1u);
  assert_only_pixels(0u,NULL,0u);
  assert(s_test_display_fixture.draw_count == 1u);
  assert(s_test_display_fixture.close_count == 1u);
  h2_lua_host_destroy(host);
  h2_runtime_deinit(runtime);
}

static void test_display_vectors(void) {
  h2_runtime_t *runtime = create_runtime();
  h2_lua_host_t *host = create_host(runtime);
  static const struct {
    const char *draw;
    const char *pixels;
  } cases[] = {
      {"d.fill_polygon({{1,1},{4,1},{4,4},{1,4}},'red')",
       "........" ".####..." ".####..." ".####..."
       "........" "........" "........" "........"},
      {"d.fill_polygon({{1,1},{5,1},{5,3},{3,3},{3,5},{1,5}},'red')",
       "........" ".#####.." ".#####.." ".###...."
       ".###...." "........" "........" "........"},
      {"d.fill_polygon({{1,1},{5,5},{1,5},{5,1}},'red')",
       "........" ".#####.." "..###..." "...#...."
       "..###..." "........" "........" "........"},
      {"d.fill_polygon({{0,0},{4,0},{4,4},{0,4}},'red',.5,1,3,.5)",
       "........" ".###...." "........" "........"
       "........" "........" "........" "........"},
      {"d.fill_polygon({{-100000,-100000},{100000,-100000},"
       "{100000,100000},{-100000,100000}},'red',0,2,5,16)",
       "........" "........" "########" "########"
       "########" "........" "........" "........"},
      {"d.fill_polygon({{1,1},{1,1},{1,1}},'red');"
       "d.fill_polygon({{1,1},{5,1},{3,1}},'red')",
       "........" "........" "........" "........"
       "........" "........" "........" "........"},
      {"d.fill_ellipse(3,3,2,2,'red')",
       "........" "...#...." ".####..." ".#####.."
       ".####..." "...#...." "........" "........"},
      {"d.fill_ellipse(3.25,3.25,1.5,1.5,'red')",
       "........" "........" "...#...." "..####.."
       "..####.." "...#...." "........" "........"},
      {"d.fill_ellipse(3,3,0,1,'red',0,3,5)",
       "........" "........" "........" "...#...."
       "...#...." "........" "........" "........"},
      {"d.draw_commands(d.compile_commands({{1,1,1,5,5,'red'}}))",
       "........" ".#......" "..#....." "...#...."
       "....#..." ".....#.." "........" "........"},
      {"d.draw_commands(d.compile_commands({{1,5,5,1,1,'red'}}))",
       "........" ".#......" "..#....." "...#...."
       "....#..." ".....#.." "........" "........"},
      {"d.draw_commands(d.compile_commands({{0,1,1,2,2,'blue'}}),"
       "0,8,0,0,1,1,'red')",
       "........" ".##....." ".##....." "........"
       "........" "........" "........" "........"},
  };
  for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    char script[1024];
    int size = snprintf(script, sizeof(script),
                        "local d=require('display');%s;d.present()", cases[i].draw);
    assert(size > 0 && (size_t)size < sizeof(script));
    (void)run_display_script(host, "@vector-pixels.lua", (const uint8_t *)script,
                             (size_t)size);
    assert(strlen(cases[i].pixels) == 64u);
    for (size_t pixel = 0u; pixel < 64u; ++pixel) {
      uint16_t expected = cases[i].pixels[pixel] == '#' ? 0xf800u : 0u;
      if (s_test_display_fixture.pixels[pixel] != expected)
        fprintf(stderr, "vector case=%zu pixel=%zu actual=%04x expected=%04x\n",
                i, pixel, s_test_display_fixture.pixels[pixel], expected);
      assert(s_test_display_fixture.pixels[pixel] == expected);
    }
    assert(s_test_display_fixture.open_count == 1u);
    assert(s_test_display_fixture.close_count == 1u);
  }
  static const uint8_t commands[] =
      "local d=require('display');local src={{0,1,1,2,2,'red'},"
      "{1,-100000,4,100000,4,'blue'}};"
      "local c=d.compile_commands(src);src[1][2]=7;src[1][6]='white';"
      "src=nil;collectgarbage('collect');d.draw_commands(c);d.present()";
  (void)run_display_script(host, "@commands-copy.lua", commands, sizeof(commands)-1u);
  for (size_t y = 0u; y < 8u; ++y) {
    for (size_t x = 0u; x < 8u; ++x) {
      uint16_t expected = y == 4u ? 0x001fu :
          ((x == 1u || x == 2u) && (y == 1u || y == 2u) ? 0xf800u : 0u);
      assert(s_test_display_fixture.pixels[y * 8u + x] == expected);
    }
  }
  static const uint8_t transformed[] =
      "local d=require('display');local c=d.compile_commands({"
      "{0,1,1,2,2,'red'},{1,-100000,4,100000,4,'blue'}});"
      "d.draw_commands(c,2,6,.5,1,2,1,'green');d.present();"
      "d.deinit();assert(not pcall(d.draw_commands,c));"
      "assert(not pcall(d.fill_ellipse,3,3,2,2,'red'));"
      "assert(not pcall(d.fill_polygon,{{0,0},{1,0},{1,1}},'red'))";
  (void)run_display_script(host, "@commands-transform.lua", transformed,
                           sizeof(transformed)-1u);
  for (size_t y = 0u; y < 8u; ++y) {
    for (size_t x = 0u; x < 8u; ++x) {
      uint16_t expected = y == 5u || ((y == 2u || y == 3u) && x >= 3u && x < 7u)
                              ? 0x0400u : 0u;
      if (s_test_display_fixture.pixels[y * 8u + x] != expected)
        fprintf(stderr, "transform pixel=%zu,%zu actual=%04x expected=%04x\n",
                x, y, s_test_display_fixture.pixels[y * 8u + x], expected);
      assert(s_test_display_fixture.pixels[y * 8u + x] == expected);
    }
  }
  static const uint8_t invalid[] =
      "local d=require('display');d.present();"
      "local p={{0,0},{4,0},{4,4}};"
      "local c=d.compile_commands({{0,0,0,8,8,'red'}});"
      "local function bad(f,...) assert(not pcall(f,...)) end;"
      "bad(d.fill_polygon,{},'red');bad(d.fill_polygon,{{0,0},{4,0},false},'red');"
      "bad(d.fill_polygon,{{0,0},{4,0},{0/0,1}},'red');"
      "bad(d.fill_polygon,p,'red',0,0,8,0);"
      "bad(d.fill_polygon,p,'red',0,0,8,17);"
      "bad(d.fill_polygon,p,'red',0,4294967296,4294967304);"
      "bad(d.fill_ellipse,1,1,-1,2,'red');bad(d.fill_ellipse,1,1,2,0,'red');"
      "bad(d.fill_ellipse,1,1,2,2049,'red');"
      "bad(d.fill_ellipse,math.huge,1,2,2,'red');"
      "bad(d.compile_commands,{{9,0,0,1,1,'red'}});"
      "bad(d.compile_commands,{{0,0,0,-.1,1,'red'}});"
      "bad(d.compile_commands,{{1,0,0,math.huge,1,'red'}});"
      "bad(d.compile_commands,{{0,0,0,1,1,'bad-color'}});"
      "bad(d.draw_commands,{});bad(d.draw_commands,c,0,8,0,0,0);"
      "bad(d.draw_commands,c,0,8,0,0,1001);"
      "bad(d.draw_commands,c,0,8,0,0,1,1,'bad-color');"
      "bad(d.draw_commands,c,4294967296,4294967304);"
      "d.draw_commands(c,3,3);d.draw_commands(d.compile_commands({}));"
      "d.draw_commands(d.compile_commands({{0,0,0,0,8,'red'}}));"
      "d.fill_polygon(p,'red',0,2,2);d.fill_ellipse(3,3,2,2,'red',0,2,2);"
      "d.present()";
  (void)run_display_script(host, "@vector-invalid.lua", invalid, sizeof(invalid)-1u);
  assert_only_pixels(0u, NULL, 0u);
  assert(s_test_display_fixture.draw_count == 1u);
  assert(s_test_display_fixture.present_count == 2u);
  static const uint8_t memory[] =
      "local d=require('display');local cmd={0,1,1,2,2,'red'};"
      "local t={};for i=1,4096 do t[i]=cmd end;"
      "collectgarbage('collect');local baseline=collectgarbage('count');"
      "local c=d.compile_commands(t);collectgarbage('collect');"
      "assert(collectgarbage('count')>baseline+80);"
      "local weak=setmetatable({c},{__mode='v'});c=nil;"
      "collectgarbage('collect');assert(weak[1]==nil);"
      "assert(collectgarbage('count')<baseline+4);"
      "for i=4097,8192 do t[i]=cmd end;collectgarbage('collect');"
      "baseline=collectgarbage('count');"
      "local ok,err=pcall(d.compile_commands,t);"
      "assert(not ok and err=='not enough memory');err=nil;"
      "collectgarbage('collect');assert(collectgarbage('count')<baseline+4);"
      "t=nil;collectgarbage('collect');"
      "d.draw_commands(d.compile_commands({cmd}));d.present()";
  (void)run_display_script(host, "@commands-memory.lua", memory, sizeof(memory)-1u);
  static const expected_pixel_t square[] = {{1,1},{2,1},{1,2},{2,2}};
  assert_only_pixels(0xf800u, square, sizeof(square)/sizeof(square[0]));
  static const uint8_t reentrant_color[] =
      "local d=require('display');local c=d.compile_commands({{0,0,0,8,8,'red'}});"
      "d.present();local color=setmetatable({}, {__index=function() "
      "d.deinit();return 255 end});"
      "assert(not pcall(d.draw_commands,c,0,8,0,0,1,1,color))";
  (void)run_display_script(host, "@commands-color-close.lua", reentrant_color,
                           sizeof(reentrant_color)-1u);
  assert_only_pixels(0u, NULL, 0u);
  assert(s_test_display_fixture.close_count == 1u);
  h2_lua_host_destroy(host);

  const h2_lua_host_config_t config = {
      .runtime = runtime, .worker_count = 1u, .max_jobs = 1u,
      .vm_memory_limit_bytes = 2u * 1024u * 1024u,
      .execution_timeout_ms = 1000u,
  };
  assert(h2_lua_host_create(&config, &host) == H2_PAL_OK);
  assert(h2_lua_host_start(host) == H2_PAL_OK);
  static const uint8_t capacity[] =
      "local d=require('display');local t={};local cmd={0,1,1,1,1,'red'};"
      "for i=1,16384 do t[i]=cmd end;local c=d.compile_commands(t);"
      "t[16385]=cmd;local ok,err=pcall(d.compile_commands,t);"
      "assert(not ok and err:find('too many pixel commands',1,true));"
      "d.draw_commands(c);d.present();"
      "t={};for i=1,128 do t[i]={1,1} end;d.fill_polygon(t,'red');"
      "t[129]={1,1};assert(not pcall(d.fill_polygon,t,'red'))";
  (void)run_display_script(host, "@commands-capacity.lua", capacity,
                           sizeof(capacity)-1u);
  static const expected_pixel_t pixel[] = {{1,1}};
  assert_only_pixels(0xf800u, pixel, 1u);
  h2_lua_host_destroy(host);
  h2_runtime_deinit(runtime);
}

static int test_region_failure(lua_State *state) {
  int draw = (int)luaL_checkinteger(state, 1);
  s_test_display_fixture.fail_draw = draw > 0 ?
      s_test_display_fixture.draw_count + (size_t)draw : 0;
  s_test_display_fixture.fail_present = lua_toboolean(state, 2);
  return 0;
}

static int test_region_finalizer(lua_State *state) {
  assert(lua_toboolean(state, 1));
  assert(s_test_display_fixture.close_count == 1u);
  ++s_test_display_fixture.finalizer_count;
  return 0;
}

static int test_region_open(void *lua_state, void *user) {
  (void)user;
  lua_State *state = lua_state;
  lua_newtable(state);
  lua_pushcfunction(state, test_region_failure);
  lua_setfield(state, -2, "fail");
  lua_pushcfunction(state, test_region_finalizer);
  lua_setfield(state, -2, "finalizer");
  return 1;
}

static void test_display_regions(void) {
  h2_runtime_t *runtime = create_runtime();
  h2_lua_host_t *host = create_unstarted_host(runtime);
  assert(h2_lua_register_module(host, "region_test", test_region_open, NULL) == H2_PAL_OK);
  assert(h2_lua_host_start(host) == H2_PAL_OK);
  static const struct { const char *draw; const char *pixels; } cases[] = {
      {"d.draw_region(r,0,0)",
       "........" ".##....." "....#..." "........"
       "........" "........" "........" "........"},
      {"d.draw_region(r,1,2,0,8,'black')",
       "BBBBBBBB" "BBBBBBBB" "BBBBBBBB" "BB##BBBB"
       "BBBBB#BB" "BBBBBBBB" "BBBBBBBB" "BBBBBBBB"},
      {"d.draw_region(r,0,0,0,8,'red')",
       "........" ".BB....." "....B..." "........"
       "........" "........" "........" "........"},
      {"d.draw_region(r,-1,-1,0,2,'black',1,3)",
       "B#BBBBBB" "BBBBBBBB" "BBBBBBBB" "BBBBBBBB"
       "BBBBBBBB" "BBBBBBBB" "BBBBBBBB" "BBBBBBBB"},
      {"d.draw_region(r,100000,-100000);d.draw_region(r,0,0,4,4)",
       "BBBBBBBB" "BBBBBBBB" "BBBBBBBB" "BBBBBBBB"
       "BBBBBBBB" "BBBBBBBB" "BBBBBBBB" "BBBBBBBB"},
  };
  for (int masked = 0; masked <= 1; ++masked) {
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
      char script[2048];
      int length = snprintf(script, sizeof(script),
          "local d=require('display');d.clear('black');"
          "d.fill_rect(1,1,2,1,'red');d.fill_rect(4,2,1,1,'red');"
          "local r=d.capture_region(0,0,8,8,%s);"
          "collectgarbage('collect');d.clear('blue');%s;d.present()",
          masked ? "'black'" : "nil", cases[i].draw);
      assert(length > 0 && (size_t)length < sizeof(script));
      (void)run_display_script(host, "@region-pixels.lua", (const uint8_t *)script,
                               (size_t)length);
      for (size_t p = 0; p < 64; ++p) {
        uint16_t expected = cases[i].pixels[p] == '#' ? 0xf800u :
                            cases[i].pixels[p] == 'B' ? 0x001fu : 0;
        if (s_test_display_fixture.pixels[p] != expected)
          fprintf(stderr, "region masked=%d case=%zu pixel=%zu got=%x expected=%x\n",
                  masked, i, p, s_test_display_fixture.pixels[p], expected);
        assert(s_test_display_fixture.pixels[p] == expected);
      }
    }
  }
  static const uint8_t invalid[] =
      "local d=require('display');d.present();local r=d.capture_region(0,0,8,8);"
      "local function bad(f,...) assert(not pcall(f,...)) end;"
      "bad(d.capture_region,-1,0,1,1);bad(d.capture_region,0,0,0,1);"
      "bad(d.capture_region,0,0,4097,1);bad(d.capture_region,0,0,9,1);"
      "bad(d.capture_region,4294967296,0,1,1);"
      "bad(d.capture_region,0,0,1,1,nil,r);bad(d.capture_region,0,0,8,8,'black',r);"
      "bad(d.draw_region,{});bad(d.draw_region,r,.5,0);"
      "bad(d.draw_region,r,0,0,-1,8);bad(d.draw_region,r,0,0,0,4294967296);"
      "bad(d.draw_region,r,0,0,0,8,nil,9,8);"
      "bad(d.restore_background,d.capture_region(0,0,1,1));"
      "bad(d.restore_background,d.capture_region(0,0,8,8,'black'));"
      "bad(d.present,{retained=1});bad(d.present,{bounds=0});"
      "bad(d.present,{merge_gap=9});bad(d.present,{merge_gap=4294967296});"
      "d.release_background();d.release_background();assert(d.present()==0)";
  (void)run_display_script(host, "@region-invalid.lua", invalid, sizeof(invalid)-1);
  assert(s_test_display_fixture.draw_count == 1);
  static const uint8_t retained[] =
      "local d=require('display');local n=require('region_test');"
      "local w,h=d.width,d.height;local function p(px,nr,opts) "
      "local a,b=d.present(opts);assert(a==px and b==nr, a..'/'..b..' expected '..px..'/'..nr) end;"
      "d.clear('blue');local bg=d.capture_region(0,0,w,h);"
      "d.restore_background(bg);p(w*h,1,{retained=true});p(0,0);"
      "d.fill_rect(1,1,1,1,'red');p(256,1);"
      "d.restore_background(bg);p(256,1);d.restore_background(bg);p(0,0);"
      "d.fill_rect(1,1,1,1,'blue');p(0,0);"
      "d.fill_rect(w-1,h-1,1,1,'red');p((w-16)*(h-32),1);"
      "d.restore_background(bg);p((w-16)*(h-32),1);"
      "d.clear('red');assert(d.capture_region(0,0,w,h,nil,bg)==bg);"
      "d.clear('black');d.restore_background(bg);p(w*h,1,{bounds=true});"
      "local weak=setmetatable({bg},{__mode='v'});bg=nil;collectgarbage('collect');"
      "assert(weak[1]);d.release_background();collectgarbage('collect');assert(not weak[1]);"
      "p(w*h,1,{retained=false});p(0,0);p(w*h,1,{retained=true});"
      "n.fail(0,true);assert(not pcall(d.present));p(w*h,1);p(0,0);"
      "d.fill_rect(0,0,1,1,'blue');d.fill_rect(w-1,h-1,1,1,'blue');"
      "n.fail(2,false);assert(not pcall(d.present));p(w*h,1);p(0,0);"
      "d.fill_rect(0,0,1,1,'red');d.fill_rect(w-1,h-1,1,1,'red');"
      "p(w*h,1,{bounds=true});p(0,0);"
      "d.deinit();d.deinit();assert(not pcall(d.present));"
      "assert(not pcall(d.draw_region,weak[1],0,0))";
  (void)run_display_script_size(host, "@retained-regions.lua", retained,
                                sizeof(retained)-1, 31, 35);
  for (size_t i = 0; i < 31u*35u; ++i)
    assert(s_test_display_fixture.pixels[i] == 0xf800u);
  assert(s_test_display_fixture.close_count == 1);
  static const uint8_t merge[] =
      "local d=require('display');d.present({retained=true});"
      "d.fill_rect(0,0,1,1,'red');d.fill_rect(32,0,1,1,'red');"
      "local p,n=d.present();assert(p==512 and n==2);"
      "d.clear('black');p,n=d.present({merge_gap=1});assert(p==768 and n==1);"
      "d.fill_rect(0,0,1,1,'red');d.fill_rect(0,32,1,1,'red');"
      "p,n=d.present();assert(p==512 and n==2);"
      "d.clear('black');p,n=d.present({merge_gap=1});assert(p==768 and n==1);"
      "d.clear('black');assert(d.present()==0)";
  (void)run_display_script_size(host, "@retained-merge.lua", merge, sizeof(merge)-1, 48, 48);
  static const uint8_t memory[] =
      "local d=require('display');local w,h=d.width,d.height;"
      "local r=d.capture_region(0,0,w,h);local weak=setmetatable({r},{__mode='v'});"
      "collectgarbage('collect');local before=collectgarbage('count');"
      "for i=1,100 do assert(d.capture_region(0,0,w,h,nil,r)==r) end;"
      "collectgarbage('collect');assert(collectgarbage('count')<before+1,'reuse allocation');"
      "d.restore_background(r);r=nil;collectgarbage('collect');assert(weak[1]);"
      "d.present({retained=true});d.release_background();collectgarbage('collect');"
      "assert(not weak[1]);"
      "d.clear('red');local held={};for i=1,100 do held[i]=false end;local oom=false;"
      "for i=1,100 do local ok,value=pcall(d.capture_region,0,0,w,h,'black');"
      "if not ok then assert(value=='not enough memory');oom=true;break end;held[i]=value end;"
      "assert(oom,'capture did not exhaust VM');held=nil;collectgarbage('collect');d.clear('black');"
      "r=d.capture_region(0,0,w,h,'black');d.draw_region(r,0,0);assert(d.present()==0);"
      "local color=setmetatable({}, {__index=function() d.deinit();return 255 end});"
      "assert(not pcall(d.draw_region,r,0,0,0,h,color));"
      "assert(not pcall(d.begin_frame,{clear=true,color='red'}))";
  (void)run_display_script_size(host, "@region-memory.lua", memory, sizeof(memory)-1, 64, 64);
  static const uint8_t drawing_paths[] =
      "local d=require('display');d.clear('blue');local bg=d.capture_region(0,0,d.width,d.height);"
      "d.restore_background(bg);d.present({retained=true});"
      "local cmd=d.compile_commands({{0,1,1,3,3,'red'}});"
      "local mesh=d.compile_mesh({{1,1},{4,1},{4,4}},{{0,1,3,'red'}});"
      "local operations={"
      "function() d.clear('red') end,"
      "function() d.fill_rect(1,1,3,3,'red') end,"
      "function() d.draw_line(1,1,4,4,'red') end,"
      "function() d.fill_polygon({{1,1},{4,1},{4,4}},'red') end,"
      "function() d.fill_ellipse(4,4,2,2,'red') end,"
      "function() d.draw_commands(cmd) end,"
      "function() d.draw_mesh(mesh) end,"
      "function() d.fill_circle_aa(4,4,2,'red') end,"
      "function() d.draw_text(1,1,'A',{color='red'}) end,"
      "function() d.fade_to_black(255) end,"
      "function() d.fade_rect_to_black(1,1,4,4,128) end,"
      "function() d.begin_frame({clear=true,color='red'});d.end_frame() end};"
      "for _,draw in ipairs(operations) do draw();d.present();"
      "d.restore_background(bg);assert(d.present()>0);"
      "d.restore_background(bg);assert(d.present()==0) end;"
      "d.clear('red');local other=d.capture_region(0,0,d.width,d.height);"
      "d.restore_background(other);d.present();d.restore_background(bg);"
      "assert(d.present()==d.width*d.height);assert(d.present()==0)";
  (void)run_display_script_size(host, "@background-drawing-paths.lua", drawing_paths,
                                sizeof(drawing_paths)-1, 31, 35);
  for (size_t i = 0; i < 31u*35u; ++i)
    assert(s_test_display_fixture.pixels[i] == 0x001fu);
  static const uint8_t close[] =
      "local d=require('display');local n=require('region_test');"
      "local bg=d.capture_region(0,0,8,8);d.restore_background(bg);d.present({retained=true});"
      "local r=d.capture_region(0,0,1,1);"
      "keep_finalizer=setmetatable({}, {__gc=function() "
      "n.finalizer(not pcall(d.draw_region,r,0,0) and not pcall(d.clear,'red')) end})";
  (void)run_display_script(host, "@regions-release-finalizer.lua", close, sizeof(close)-1);
  assert(s_test_display_fixture.finalizer_count == 1);
  /* Host destruction has a distinct release entry point. */
  test_display_reset();
  h2_lua_job_id_t job;
  assert(h2_lua_job_submit_text(host, NULL, "@regions-host-finalizer.lua", close,
                               sizeof(close)-1, NULL, 0, &job) == H2_PAL_OK);
  run_until_terminal(host, job, 64);
  assert(status(host, job).state == H2_LUA_JOB_SUCCEEDED);
  h2_lua_host_destroy(host);
  assert(s_test_display_fixture.finalizer_count == 1);
  h2_runtime_deinit(runtime);
}

static void test_display_strokes(void) {
  h2_runtime_t *runtime = create_runtime();
  h2_lua_host_t *host = create_unstarted_host_with_scheduler(runtime, 1000, 0, 5000, 8192);
  assert(h2_lua_host_start(host) == H2_PAL_OK);
  static const uint8_t reference[] =
      "local d=require('display');math.randomseed(354);"
      "local function polygon(p,color,offset,top,bottom) "
      "for y=top,bottom-1 do local xs={};for i=1,#p do local a,b=p[i],p[i%#p+1];"
      "if (a[2]<=y and b[2]>y) or (b[2]<=y and a[2]>y) then "
      "xs[#xs+1]=a[1]+(y-a[2])*(b[1]-a[1])/(b[2]-a[2]) end end;table.sort(xs);"
      "for i=1,#xs-1,2 do local edge=math.ceil(xs[i]);local x=math.floor(edge+offset+.5);"
      "local r=x+math.floor(xs[i+1])-edge;local a,b=math.max(0,x),math.min(d.width-1,r);"
      "if a<=b then d.fill_rect(a,y,b-a+1,1,color) end end end end;"
      "for k=1,500 do local p={};for i=1,3+k%6 do "
      "local x,y=math.random()*20-6,math.random()*20-6;"
      "if k%3==0 then x=math.floor(x)+1e-8;y=math.floor(y)-1e-8 end;p[i]={x,y} end;"
      "local offset=k%2==0 and .5 or -.4;local top=k%3;"
      "d.clear('black');polygon(p,'red',offset,top,8);d.present({retained=true});"
      "d.clear('black');d.fill_polygon(p,'red',offset,top,8);"
      "assert(d.present()==0,'polygon '..k) end;"
      "local fast_total=0;"
      "for k=1,200 do local p={{1+math.random()*5,1+math.random()*5},"
      "{1+math.random()*5,1+math.random()*5},{1+math.random()*5,1+math.random()*5}};"
      "local widths={.3+math.random()*3,.3+math.random()*3};local colors={'red','blue'};"
      "d.clear('black');for i=1,2 do local a,b=p[i],p[i+1];local dx,dy=b[1]-a[1],b[2]-a[2];"
      "local len=math.sqrt(dx*dx+dy*dy);local w=widths[i];"
      "if len<.01 then local x,y=math.floor(a[1]-w/2+.5),math.floor(a[2]-w/2+.5);"
      "local side=math.floor(w+.5);if side>0 then d.fill_rect(x,y,side,side,colors[i]) end "
      "else local nx,ny=(-dy/len)*w/2,(dx/len)*w/2;"
      "polygon({{a[1]+nx,a[2]+ny},{b[1]+nx,b[2]+ny},{b[1]-nx,b[2]-ny},{a[1]-nx,a[2]-ny}},colors[i],0,0,8);"
      "d.draw_line(math.floor(a[1]+.5),math.floor(a[2]+.5),math.floor(b[1]+.5),math.floor(b[2]+.5),colors[i]) end end;"
      "d.present();d.clear('black');d.stroke_path(p,widths,colors);assert(d.present()==0,'hard '..k);"
      "d.clear('black');local hit,fast=d.stroke_path(p,widths,colors,0,0,8,true,true);"
      "assert(not hit);fast_total=fast_total+fast;assert(d.present()==0,'fast '..k);"
      "d.clear('black');hit=d.stroke_path(p,widths,colors,0,0,8,true,true);"
      "assert(hit);assert(d.present()==0,'hot '..k) end;assert(fast_total>0)";
  (void)run_display_script(host, "@stroke-reference.lua", reference, sizeof(reference)-1);
  static const uint8_t keys[] =
      "local d=require('display');local p,w={{1,1},{6,6}},{2};"
      "local function draw(color,off,top,bot,fast,scale) "
      "return d.stroke_path(p,w,color,off,top,bot,true,fast,false,scale) end;"
      "assert(not draw('red',0,0,8,false,1));assert(draw('red',0,0,8,false,1));"
      "assert(not draw('blue',0,0,8,false,1));assert(draw('blue',0,0,8,false,1));"
      "assert(not draw('blue',.5,0,8,false,1));assert(not draw('blue',.5,1,8,false,1));"
      "assert(not draw('blue',.5,1,7,false,1));assert(not draw('blue',.5,1,7,true,1));"
      "assert(not draw('blue',.5,1,7,true,.8));w[1]=3;"
      "assert(not draw('blue',.5,1,7,true,.8));p[1][1]=2;"
      "assert(not draw('blue',.5,1,7,true,.8));assert(draw('blue',.5,1,7,true,.8));"
      "local weak=setmetatable({p,w},{__mode='v'});p=nil;w=nil;collectgarbage('collect');"
      "assert(not weak[1] and not weak[2]);"
      "local function bad(...) assert(not pcall(d.stroke_path,...)) end;"
      "bad({}, {},'red');bad({{0,0},{1,1}},{},'red');bad({{0,0},{1,1}},{-1},'red');"
      "bad({{0,0},{1,1}},{1001},'red');bad({{0,0},{0/0,1}},{1},'red');"
      "p={{1,1},{4,4}};w={1};bad(p,w,'red',0,0,4294967296);"
      "bad(p,w,'red',0,0,8,1);bad(p,w,'red',0,0,8,false,false,false,0);"
      "bad(p,w,'red',0,0,8,false,false,true,1,.26);"
      "local color=setmetatable({}, {__index=function() d.deinit();return 255 end});bad(p,w,color)";
  (void)run_display_script(host, "@stroke-keys.lua", keys, sizeof(keys)-1);
  static const uint8_t smooth[] =
      "local d=require('display');local p={{1.5,3.5},{5.5,3.5}};"
      "d.clear('black');d.stroke_path(p,{1},'red',0,0,8,false,false,true);"
      "d.present({retained=true});"
      "d.clear('black');d.stroke_path({p[1],{3.5,3.5},p[2]},{1,1},'red',0,0,8,false,false,true,1,.1);"
      "assert(d.present()==0);d.clear('black');"
      "d.stroke_path({p[1],p[2],p[1]},{1,1},{'red','blue'},0,0,8,false,false,true);"
      "assert(d.present()==0);"
      "d.clear('black');d.stroke_path({{3,3},{3,3}},{2},'red');d.present();"
      "d.clear('black');local hit=d.stroke_path({{3,3},{3,3}},{2},'red',0,0,8,true,true);"
      "assert(not hit);assert(d.present()==0);"
      "d.clear('black');d.stroke_path(p,{0},'red',0,0,8,false,false,true);d.present();"
      "d.clear('black');assert(d.present()==0)";
  (void)run_display_script(host, "@stroke-smooth.lua", smooth, sizeof(smooth)-1);
  static const uint8_t overflow[] =
      "local d=require('display');local p,w={},{};"
      "for i=1,256 do p[i]={i%2==0 and 2 or 5,i%2==0 and 62 or 1};if i<256 then w[i]=2 end end;"
      "d.stroke_path(p,w,'red',0,0,64,false,true);d.present({retained=true});"
      "d.clear('black');assert(not d.stroke_path(p,w,'red',0,0,64,true,true));"
      "assert(d.present()==0);d.clear('black');"
      "assert(not d.stroke_path(p,w,'red',0,0,64,true,true));assert(d.present()==0);"
      "p[257]={1,1};w[256]=2;assert(not pcall(d.stroke_path,p,w,'red'));"
      "local memory=collectgarbage('count');p=nil;w=nil;collectgarbage('collect');"
      "assert(collectgarbage('count')<memory-30);"
      "local points={{-100000,4},{100000,4}};d.clear('black');"
      "d.stroke_path(points,{1},'red',0,0,64,false,false,true);d.present();"
      "d.clear('black');d.stroke_path({{-1000,4},{1000,4}},{1},'red',0,0,64,false,false,true);"
      "assert(d.present()==0)";
  (void)run_display_script_size(host, "@stroke-overflow.lua", overflow, sizeof(overflow)-1, 64, 64);
  static const uint8_t smooth_memory[] =
      "local d=require('display');d.clear('blue');d.present({retained=true});"
      "local held={};for i=1,100 do held[i]=false end;local oom=false;"
      "for i=1,100 do local ok,value=pcall(d.capture_region,0,0,64,64);"
      "if not ok then assert(value=='not enough memory');oom=true;break end;held[i]=value end;"
      "assert(oom);local p,w={{0,0},{63,63}},{100};"
      "local ok,err=pcall(d.stroke_path,p,w,'red',0,0,64,false,false,true);"
      "assert(not ok and err=='not enough memory');assert(d.present()==0);"
      "held=nil;collectgarbage('collect');d.stroke_path(p,w,'red',0,0,64,false,false,true);"
      "assert(d.present()>0);collectgarbage('collect');local before=collectgarbage('count');"
      "d.deinit();collectgarbage('collect');assert(collectgarbage('count')<before-18)";
  (void)run_display_script_size(host, "@stroke-smooth-memory.lua", smooth_memory,
                                sizeof(smooth_memory)-1, 64, 64);
  h2_lua_host_destroy(host);
  h2_runtime_deinit(runtime);
}

static void test_display_mesh_identity(void) {
  h2_runtime_t *runtime = create_runtime();
  h2_lua_host_t *host = create_unstarted_host_with_scheduler(runtime,1000,0,5000,8192);
  assert(h2_lua_register_module(host,"mesh_test",test_mesh_open,NULL) == H2_PAL_OK);
  assert(h2_lua_host_start(host) == H2_PAL_OK);
  /* Reference positions evaluate the original binary64 expression in Lua.
   * Compare the complete framebuffer, not a few selected sample pixels. */
  static const uint8_t pixels[] =
      "local d=require('display');local m=d.compile_mesh({},{},6,2);"
      "local ref=d.compile_mesh({},{},6,2);local p={{0,1,4,'red'},{1,5,2,'blue'}};"
      "local matrices={{1.,0.,0.,1.,0.,0.},{1.,-0.,-0.,1.,-0.,-0.},"
      "{1.+2^-52,0.,0.,1.,0.,0.},{1.-2^-53,0.,0.,1.,0.,0.},"
      "{1.,0.,2^-52,1.,0.,0.},{1.,2^-52,0.,1.,0.,0.},"
      "{1.,0.,0.,1.,2^-52,0.},{1.,0.,0.,1.,0.,2^-52},"
      "{-1.,0.,0.,1.,6.,0.},{1.,0.,0.,1.,0.,0.},"
      "{1.,0.,0.,1.,0.,0.},{1.,0.,0.,1.,0.,0.}};"
      "for frame=1,16 do local f=(frame%4)*.25;"
      "local v={{-.5+f,1.5},{6.5-f,-.5},{7.5,5.5-f},{1.5,7.5},"
      "{-1.+f,7.-f},{8.-f,0.+f}};"
      "if frame%8==0 then v[1]={-0.,-0.};v[5]={-0.,2^-1074};"
      "v[6]={7.,-2^-1074};end;d.update_mesh(m,v,p);"
      "for k,a in ipairs(matrices) do local grid=(k==9 or k==11) and 2 or 0;local rv={};"
      "for i,vv in ipairs(v) do local x=(a[1]*vv[1]+a[3]*vv[2])+a[5];"
      "local y=(a[2]*vv[1]+a[4]*vv[2])+a[6];"
      "if grid~=0 then x=math.floor(x/grid+.5)*grid;y=math.floor(y/grid+.5)*grid end;"
      "rv[i]={x,y};end;d.update_mesh(ref,rv,p);"
      "local opts={offset_x=f-.5,left=frame%2,right=8-frame%3,"
      "top=frame%3,bottom=8-frame%2,color=frame%2==0 and 'white' or nil};"
      "d.clear('black');d.draw_mesh(ref,opts);d.present({retained=true});"
      "opts.matrix=a;opts.grid=grid;"
      /* Rebuild, capture, replay, uncached coordinate hit, then replay again. */
      "for pass=1,5 do opts.cache=pass~=1 and pass~=4;"
      "d.clear('black');d.draw_mesh(m,opts);"
      "assert(d.present()==0,'identity pixels '..frame..'/'..k..'/'..pass);end;"
      "end;collectgarbage('collect');end";
  (void)run_display_script(host,"@mesh-identity-pixels.lua",pixels,sizeof(pixels)-1);
  static const uint8_t recovery[] =
      "local d=require('display');local n=require('mesh_test');local m=n.new();"
      "local v={{1,1},{4,1},{4,4},{1,4},{1000000,-1000000},{-0.,0.}};"
      "local p={{0,1,4,'red'}};d.update_mesh(m,v,p);"
      "for _,opts in ipairs({{cache=true},{cache=true,matrix={-1,0,0,1,6,0}}}) do "
      "d.clear('black');d.draw_mesh(m,opts);d.present({retained=true});"
      /* A late, unused vertex fails after earlier vertices were transformed. */
      "assert(not pcall(d.draw_mesh,m,{cache=true,matrix={32,0,0,1,0,0}}));"
      "assert(d.present()==0);d.clear('black');d.draw_mesh(m,opts);"
      "assert(d.present()==0);n.update(m,0);d.clear('black');d.draw_mesh(m,opts);"
      "assert(d.present()==0);end;"
      "local ref=d.compile_mesh({{1,6},{6,6}},{{1,1,2,'blue'}});"
      "d.clear('black');d.draw_mesh(ref);d.present();n.update(m,1);"
      "for i=1,3 do d.clear('black');d.draw_mesh(m,{matrix={1,0,0,1,0,0},"
      "grid=0,cache=true});assert(d.present()==0);end;"
      "n.update(m,2);d.clear('black');d.present();d.draw_mesh(m,{cache=true});"
      "assert(d.present()==0);d.draw_mesh(d.compile_mesh({},{}));"
      "assert(d.present()==0);collectgarbage('collect');d.deinit();"
      "assert(not pcall(d.draw_mesh,m,{matrix={1,0,0,1,0,0},cache=true}))";
  (void)run_display_script(host,"@mesh-identity-recovery.lua",recovery,sizeof(recovery)-1);
  h2_lua_host_destroy(host);
  h2_runtime_deinit(runtime);
}

static void test_display_mesh_cache(void) {
  h2_runtime_t *runtime = create_runtime();
  h2_lua_host_t *host = create_unstarted_host_with_scheduler(runtime,1000,0,5000,8192);
  assert(h2_lua_register_module(host,"mesh_test",test_mesh_open,NULL) == H2_PAL_OK);
  assert(h2_lua_host_start(host) == H2_PAL_OK);
  static const uint8_t cache[] =
      "local d=require('display');local n=require('mesh_test');local m=n.new();"
      "local options={{},{offset_x=.5},{left=2},{right=3},{top=2},{bottom=2},"
      "{color='blue'},{matrix={-1,0,0,1,6,0}},{grid=2},{grid=0}};"
      "for _,opts in ipairs(options) do d.clear('black');d.draw_mesh(m,opts);d.present({retained=true});"
      "opts.cache=true;d.clear('black');d.draw_mesh(m,opts);assert(d.present()==0);"
      "collectgarbage('collect');local before=collectgarbage('count');"
      "d.clear('black');d.draw_mesh(m,opts);assert(d.present()==0);"
      "collectgarbage('collect');assert(collectgarbage('count')<before+1);end;"
      "d.clear('black');d.draw_mesh(m,{cache=true});d.present();"
      "n.update(m,0);d.clear('black');d.draw_mesh(m,{cache=true});assert(d.present()==0);"
      "n.update(m,1);d.clear('black');d.draw_mesh(m);d.present();"
      "d.clear('black');d.draw_mesh(m,{cache=true});assert(d.present()==0);"
      "d.update_mesh(m,{{1,1},{6,6}},{{1,1,2,'red'}});d.clear('black');d.draw_mesh(m);d.present();"
      "d.clear('black');d.draw_mesh(m,{cache=true});assert(d.present()==0);"
      "assert(not pcall(d.draw_mesh,m,{cache=true,matrix={0/0,0,0,1,0,0}}));"
      "d.clear('black');d.draw_mesh(m,{cache=true});assert(d.present()==0);"
      "n.update(m,2);d.clear('black');d.draw_mesh(m,{cache=true});d.present();"
      "assert(d.present()==0);assert(not pcall(d.draw_mesh,m,{cache=1}));"
      "local weak=setmetatable({m},{__mode='v'});m=nil;collectgarbage('collect');assert(not weak[1])";
  (void)run_display_script(host,"@mesh-cache.lua",cache,sizeof(cache)-1);
  assert_only_pixels(0,NULL,0);
  h2_lua_host_destroy(host);
  const h2_lua_host_config_t config = {.runtime=runtime,.worker_count=1,.max_jobs=1,
      .vm_memory_limit_bytes=4u*1024u*1024u,.execution_timeout_ms=5000};
  assert(h2_lua_host_create(&config,&host) == H2_PAL_OK);
  assert(h2_lua_host_start(host) == H2_PAL_OK);
  static const uint8_t capacity[] =
      "local d=require('display');local m=d.compile_mesh({},{},65536,4096);"
      "d.update_mesh(m,{{1,1},{6,1},{6,7},{1,7}},{{0,1,4,'red'}});"
      "d.draw_mesh(m,{cache=true});m=nil;collectgarbage('collect');"
      "local p={};for i=1,2048 do p[i]={0,1,4,i%2==0 and 'red' or 'blue'} end;"
      "m=d.compile_mesh({{1,1},{6,1},{6,7},{1,7}},p);"
      "d.clear('black');d.draw_mesh(m);d.present({retained=true});"
      "d.clear('black');d.draw_mesh(m,{cache=true});assert(d.present()==0);"
      "d.clear('black');d.draw_mesh(m,{cache=true});assert(d.present()==0);"
      "d.update_mesh(m,{{2,2},{5,2},{5,4},{2,4}},{{0,1,4,'blue'}});"
      "d.clear('black');d.draw_mesh(m);d.present();"
      "d.clear('black');d.draw_mesh(m,{cache=true});assert(d.present()==0)";
  (void)run_display_script(host,"@mesh-span-capacity.lua",capacity,sizeof(capacity)-1);
  h2_lua_host_destroy(host);
  h2_runtime_deinit(runtime);
}

static void test_job_results(h2_lua_host_t *host) {
  size_t invalid_size = 99;
  int invalid_present = 1;
  assert(h2_lua_job_get_result(host, H2_LUA_JOB_ID_NONE, NULL, 0,
      &invalid_size, &invalid_present) == H2_PAL_ERR_INVALID_ARG);
  assert(invalid_size == 0 && invalid_present == 0);
  assert(h2_lua_job_get_result(host, UINT32_MAX, NULL, 0,
      &invalid_size, &invalid_present) == H2_PAL_ERR_NOT_FOUND);
  assert(h2_lua_job_get_result(NULL, 1, NULL, 0,
      &invalid_size, &invalid_present) == H2_PAL_ERR_INVALID_ARG);
  const char *scripts[] = {"return 'first', 'last'", "return 42", "return nil",
      "return", "return ''", "return {}", "return true",
      "return setmetatable({}, {__tostring=function() return 'custom' end})",
      "return 'a'..string.char(0)..'b'"};
  const char *expected[] = {"first", "42", "nil", "", "", "table: ", "true",
                            "custom", "a\0b"};
  for (size_t i = 0; i < sizeof(scripts) / sizeof(scripts[0]); ++i) {
    h2_lua_job_id_t id;
    assert(h2_lua_job_submit_text(host, NULL, "@result.lua",
        (const uint8_t *)scripts[i], strlen(scripts[i]), NULL, 0, &id) == H2_PAL_OK);
    run_until_terminal(host, id, 1000u);
    assert(status(host, id).state == H2_LUA_JOB_SUCCEEDED);
    char buffer[128] = "untouched";
    size_t size = 999;
    int present = -1;
    assert(h2_lua_job_get_result(host, id, NULL, 0, &size, &present) == H2_PAL_OK);
    assert(present == (i != 3));
    if (present) {
      assert(h2_lua_job_get_result(host, id, buffer, size, &size, &present) ==
             H2_PAL_ERR_NO_SPACE);
      assert(strcmp(buffer, "untouched") == 0);
    }
    assert(h2_lua_job_get_result(host, id, buffer, sizeof(buffer), &size,
                               &present) == H2_PAL_OK);
    if (i == 5) {
      assert(size > 7 && strncmp(buffer, expected[i], 7) == 0);
    } else {
      size_t expected_size = i == 8 ? 3 : strlen(expected[i]);
      assert(size == expected_size);
      assert(memcmp(buffer, expected[i], size) == 0 && buffer[size] == 0);
    }
    assert(status(host, id).memory_used > size);
    assert(h2_lua_job_release(host, id) == H2_PAL_OK);
    assert(h2_lua_job_get_result(host, id, buffer, sizeof(buffer), &size,
                               &present) == H2_PAL_ERR_NOT_FOUND);
    assert(size == 0 && present == 0);
  }
}

int main(int argc, char **argv) {
  if (argc == 2 && strcmp(argv[1], "--prepared-benchmark") == 0) {
    test_display_raster2d(1, "libs/lua/tests/geometry_batches.lua");
    return 0;
  }
  if (argc == 2 && strcmp(argv[1], "--raster-benchmark") == 0) {
    test_display_raster2d(1, "libs/lua/tests/raster2d.lua");
    return 0;
  }
  test_display_raster2d(0, "libs/lua/tests/raster2d.lua");
  test_display_raster2d(0, "libs/lua/tests/geometry_batches.lua");
  test_display_raster2d(0, "libs/lua/tests/stroke_buffer.lua");
  test_display_mesh_identity();
  test_display_mesh_cache();
  test_display_raster2d(1, "libs/lua/tests/mesh_source_paths.lua");
  test_display_strokes();
  test_display_regions();
  test_display_meshes();
  test_display_vectors();
  test_borrowed_display();
  static const char *const esp_claw_ids[] = {
      "adc",
      "gpio",
      "i2c",
      "mcpwm",
      "pcnt",
      "rmt",
      "touch",
      "uart",
      "audio",
      "board_manager",
      "button",
      "ble",
      "ble_hid",
      "camera",
      "capability",
      "delay",
      "display",
      "environmental_sensor",
      "event_publisher",
      "http_server",
      "json",
      "image",
      "thread",
      "imu",
      "ir",
      "knob",
      "lcd",
      "lcd_touch",
      "ledc",
      "led_strip",
      "lvgl",
      "magnetometer",
      "sci",
      "storage",
      "system",
      "vision",
  };
  static const char *const extension_ids[] = {"runtime"};
  static const uint8_t embedded_nul_source[] = {'r', 'e', 't', 'u',  'r',
                                                'n', ' ', '1', '\0', '2'};
  static const uint8_t system_profile_script[] =
      "local v=require('vmath');local g=require('geometry');"
      "assert(#v.buffer(3)==3 and type(g.affine3)=='function');"
      "local s=require('system');local d=require('delay');d.delay_us(10);"
      "local delay_ok=pcall(d.delay_us,1000001);local i=s.info();local "
      "ok,e=pcall("
      "s.heap.get_info,0);return type(s.time())=='number' and "
      "type(s.date())=='string' and type(s.millis())=='number' and "
      "type(s.uptime())=='number' and s.ip()==nil and "
      "type(i)=='table' and type(i.uptime_s)=='number' and not delay_ok and "
      "not ok and "
      "string.find(e,'unsupported',1,true) and 'system-ok' or 'bad'";
  static const uint8_t component_profile_script[] =
      "local r=require('runtime');local d=require('display');"
      "assert(d==require('display') and d.width==8 and d.height==8,'display');"
      "local t=require('lcd_touch');local ti=t.sync();"
      "assert(ti.pressed and ti.just_pressed and ti.x==12 and "
      "ti.y==34,'touch');"
      "local "
      "b=assert(r.components.get(7));assert(type(b.get_key_level())=='number','"
      "button');"
      "local a=require('audio');local "
      "input_arg_ok=pcall(a.new_input,{});"
      "assert(not input_arg_ok,'input-rejects-args');"
      "local "
      "input=assert(a.new_input());local ii=input:info();"
      "assert(ii.opened and ii.role=='input' and ii.sample_rate==16000 and "
      "ii.channels==1 and ii.frame_samples==2,'input-info');"
      "local rms,peak,brightness=input:level();"
      "assert(rms>0.49 and rms<0.51 and peak==0.5 and brightness>0.70 and "
      "brightness<0.71,'input-level');"
      "assert(#input:read()==4,'input-read');assert(input:close(),'input-close'"
      ");"
      "assert(input:close(),'input-close-idempotent');local "
      "bad=select(1,a.new_output({bits_per_sample=8}));"
      "assert(bad==nil,'invalid audio');local "
      "o1=assert(a.new_output({sample_rate=16000,channels=1,bits_per_sample=16}"
      "));"
      "local "
      "o2=assert(a.new_output({sample_rate=22050,channels=1,bits_per_sample=16}"
      "));local "
      "ai=o1:info();"
      "assert(ai.opened and "
      "ai.bits_per_sample==16 and ai.frame_samples==2 and "
      "ai.bytes_per_frame==2,'info');"
      "assert(o1:write(string.char(1,2,3,4,5,6)),'write-carry');"
      "assert(o1:write(string.char(7,8,9,10)),'"
      "write1');"
      "assert(o2:write(string.char(100,101,102,103)),'write2');"
      "assert(o1:close(),'"
      "close1');"
      "assert(o2:info().opened,'independent');assert(o2:close(),'close2');"
      "assert(o1:close(),'idempotent close');"
      "local missing=select(1,r.components.get(999));"
      "return missing==nil and 'components-ok' or 'bad'";
  static const uint8_t script[] =
      "local delay=require('delay')\n"
      "local runtime=require('runtime')\n"
      "local n=0\n"
      "local "
      "removed=runtime.components.on(7,runtime.event.BUTTON_ACTION,function() "
      "n=n+10000 end)\n"
      "assert(runtime.components.off(removed) and not "
      "runtime.components.off(removed))\n"
      "local "
      "click_subscription=runtime.components.on(7,runtime.event.BUTTON_ACTION,"
      "function(e) "
      "if e.component_id==7 and type(e.component_kind)=='number' and "
      "e.pressed_at_ms==1 and e.released_at_ms==2 and e.gesture_kind==2 and "
      "e.duration_ms==1 then "
      "n=n+1 elseif e.released_at_ms==0 and e.gesture_kind==1 and "
      "e.duration_ms==0 then n=n+8 "
      "elseif e.released_at_ms==0 and e.gesture_kind==3 and "
      "e.duration_ms==500 then n=n+1024 "
      "elseif e.released_at_ms==610 and e.gesture_kind==3 and "
      "e.duration_ms==600 then n=n+2048 end end)\n"
      "assert(click_subscription~=removed and not "
      "runtime.components.off(removed))\n"
      "runtime.components.on(7,runtime.event.BUTTON_DOWN,function(e) "
      "runtime.yield();for i=1,5000 do end;"
      "if e.pressed_at_ms==11 then n=n+2 end end)\n"
      "runtime.components.on(7,runtime.event.BUTTON_UP,function(e) "
      "if e.pressed_at_ms==11 and e.released_at_ms==12 then n=n+4 end end)\n"
      "runtime.components.on(8,runtime.event.NFC_STATE,function(e) "
      "if e.component_id==8 and e.uid=='ab' then n=n+16 end end)\n"
      "runtime.components.on(9,runtime.event.IMU_GESTURE,function(e) "
      "if e.gesture_kind==1 and e.magnitude_mg==123 then n=n+32 "
      "elseif e.gesture_kind==2 and e.x_mg==1 and e.y_mg==2 and e.z_mg==3 "
      "then n=n+64 elseif e.gesture_kind==3 and e.gyro_z_mdps==4 then n=n+128 "
      "elseif e.gesture_kind==4 and e.duration_ms==5 and e.magnitude_mg==6 "
      "then n=n+256 end end)\n"
      "runtime.components.on(10,runtime.event.ERROR,function(e) "
      "if e.component_id==10 and e.result==-1 then n=n+512 end end)\n"
      "delay.delay_ms(20)\n"
      "return args.value .. ':' .. tostring(n)\n";
  static const h2_lua_arg_t args[] = {{"value", "ready"}};
  h2_runtime_t *runtime = create_runtime();
  assert_missing_worker_apis_are_rejected(runtime);
  h2_lua_host_t *invalid_size_host =
      create_unstarted_host_with_scheduler(runtime, 1000u, 0u, 1000u, SIZE_MAX);
  assert(h2_lua_host_start(invalid_size_host) == H2_PAL_OK);
  h2_lua_job_id_t invalid_size_job_id = H2_LUA_JOB_ID_NONE;
  static const uint8_t invalid_size_script[] = "require('invalid_size')";
  assert(h2_lua_job_submit_text(
             invalid_size_host, NULL, "@scripts/invalid-size-test.lua",
             invalid_size_script, sizeof(invalid_size_script) - 1u, NULL, 0u,
             &invalid_size_job_id) == H2_PAL_OK);
  run_until_terminal(invalid_size_host, invalid_size_job_id, 16u);
  assert(status(invalid_size_host, invalid_size_job_id).state ==
         H2_LUA_JOB_FAILED);
  if (strstr(status(invalid_size_host, invalid_size_job_id).message,
             "source size is invalid") == NULL)
    fprintf(stderr, "invalid-size actual: %s\n",
            status(invalid_size_host, invalid_size_job_id).message);
  assert(strstr(status(invalid_size_host, invalid_size_job_id).message,
                "source size is invalid") != NULL);
  assert(h2_lua_job_release(invalid_size_host, invalid_size_job_id) ==
         H2_PAL_OK);
  h2_lua_host_destroy(invalid_size_host);
  h2_lua_host_t *host = create_host(runtime);
  test_job_results(host);
  h2_lua_job_id_t job_id = H2_LUA_JOB_ID_NONE;
  static const h2_lua_arg_t file_args[] = {{"value", "ok"}};
  h2_runtime_button_action_event_t click = {
      .pressed_at_ms = 1u,
      .released_at_ms = 2u,
  };
  h2_runtime_button_down_event_t down = {11u};
  h2_runtime_button_up_event_t up = {11u, 12u};
  h2_runtime_nfc_state_t nfc = {
      .uid_len = 2u,
      .uid = {'a', 'b'},
  };
  h2_runtime_imu_gesture_event_t imu_shake = {
      .kind = H2_RUNTIME_IMU_GESTURE_SHAKE,
      .gesture.shake = {123, 5u},
  };
  h2_runtime_imu_gesture_event_t imu_tilt = {
      .kind = H2_RUNTIME_IMU_GESTURE_TILT,
      .gesture.tilt = {1, 2, 3},
  };
  h2_runtime_imu_gesture_event_t imu_flip = {
      .kind = H2_RUNTIME_IMU_GESTURE_FLIP,
      .gesture.flip = {4},
  };
  h2_runtime_imu_gesture_event_t imu_fall = {
      .kind = H2_RUNTIME_IMU_GESTURE_FREE_FALL,
      .gesture.free_fall = {5u, 6},
  };
  h2_pal_result_t component_error = H2_PAL_ERR_INVALID_ARG;
  h2_runtime_event_t click_event = {
      .kind = H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION,
      .component = H2_RUNTIME_COMPONENT_BUTTON,
      .component_id = 7u,
      .sequence = 1u,
      .timestamp_ms = 2u,
      .payload = &click,
      .payload_capacity = sizeof(click),
      .payload_size = sizeof(click),
  };
  h2_runtime_event_t nfc_event = {
      .kind = H2_RUNTIME_COMPONENT_EVENT_NFC_STATE,
      .component = H2_RUNTIME_COMPONENT_NFC_READER,
      .component_id = 8u,
      .sequence = 2u,
      .timestamp_ms = 3u,
      .payload = &nfc,
      .payload_capacity = sizeof(nfc),
      .payload_size = sizeof(nfc),
  };
  h2_runtime_event_t down_event = {
      .kind = H2_RUNTIME_COMPONENT_EVENT_BUTTON_DOWN,
      .component = H2_RUNTIME_COMPONENT_BUTTON,
      .component_id = 7u,
      .payload = &down,
      .payload_capacity = sizeof(down),
      .payload_size = sizeof(down),
  };
  h2_runtime_event_t up_event = {
      .kind = H2_RUNTIME_COMPONENT_EVENT_BUTTON_UP,
      .component = H2_RUNTIME_COMPONENT_BUTTON,
      .component_id = 7u,
      .payload = &up,
      .payload_capacity = sizeof(up),
      .payload_size = sizeof(up),
  };
  h2_runtime_event_t imu_event = {
      .kind = H2_RUNTIME_COMPONENT_EVENT_IMU_GESTURE,
      .component = H2_RUNTIME_COMPONENT_IMU,
      .component_id = 9u,
      .sequence = 3u,
      .timestamp_ms = 4u,
      .payload = &imu_shake,
      .payload_capacity = sizeof(imu_shake),
      .payload_size = sizeof(imu_shake),
  };
  h2_runtime_event_t error_event = {
      .kind = H2_RUNTIME_COMPONENT_EVENT_ERROR,
      .component = H2_RUNTIME_COMPONENT_BUZZER,
      .component_id = 10u,
      .sequence = 4u,
      .timestamp_ms = 5u,
      .payload = &component_error,
      .payload_capacity = sizeof(component_error),
      .payload_size = sizeof(component_error),
  };

  assert(h2_lua_job_submit_text(host, NULL, "@embedded-nul.lua",
                                embedded_nul_source,
                                sizeof(embedded_nul_source), NULL, 0u,
                                &job_id) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_lua_job_submit_file(host, NULL, "../escape.lua", NULL, 0u,
                                &job_id) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_lua_job_submit_file(host, NULL, "/absolute.lua", NULL, 0u,
                                &job_id) == H2_PAL_ERR_INVALID_ARG);
  const h2_pal_fs_api_t *fs = runtime->fs;
  runtime->fs = NULL;
  assert(h2_lua_job_submit_file(host, NULL, "scripts/main.lua", NULL, 0u,
                                &job_id) == H2_PAL_ERR_UNSUPPORTED);
  runtime->fs = fs;
  assert(h2_lua_job_submit_file(host, NULL, "scripts/oversize.lua", NULL, 0u,
                                &job_id) == H2_PAL_ERR_NO_SPACE);
  assert(h2_lua_job_submit_file(host, NULL, "scripts/bytecode.lua", NULL, 0u,
                                &job_id) == H2_PAL_ERR_FORMAT);
  assert(h2_lua_job_submit_file(host, NULL, "scripts/malformed.lua", NULL, 0u,
                                &job_id) == H2_PAL_OK);
  assert(status(host, job_id).state == H2_LUA_JOB_FAILED);
  assert(h2_lua_job_release(host, job_id) == H2_PAL_OK);
  assert(h2_lua_job_submit_file(host, NULL, "scripts/main.lua", file_args, 1u,
                                &job_id) == H2_PAL_OK);
  run_until_terminal(host, job_id, 16u);
  assert(status(host, job_id).state == H2_LUA_JOB_SUCCEEDED);
  assert(strcmp(status(host, job_id).message, "file:ok") == 0);
  assert(h2_lua_job_release(host, job_id) == H2_PAL_OK);

  assert(h2_lua_job_submit_text(host, NULL, "@system-profile.lua",
                                system_profile_script,
                                sizeof(system_profile_script) - 1u, NULL, 0u,
                                &job_id) == H2_PAL_OK);
  run_until_terminal(host, job_id, 16u);
  assert(status(host, job_id).state == H2_LUA_JOB_SUCCEEDED);
  assert(strcmp(status(host, job_id).message, "system-ok") == 0);
  assert(h2_lua_job_release(host, job_id) == H2_PAL_OK);

  atomic_store(&s_test_audio_close_count, 0);
  atomic_store(&s_test_audio_start_count, 0);
  atomic_store(&s_test_audio_stop_count, 0);
  atomic_store(&s_test_audio_mic_start_count, 0);
  atomic_store(&s_test_audio_mic_stop_count, 0);
  s_test_audio_written_bytes = 0u;
  s_test_audio_frame_count = 0u;
  assert(h2_lua_job_submit_text(host, NULL, "@component-profile.lua",
                                component_profile_script,
                                sizeof(component_profile_script) - 1u, NULL, 0u,
                                &job_id) == H2_PAL_OK);
  run_until_terminal(host, job_id, 32u);
  assert(status(host, job_id).state == H2_LUA_JOB_SUCCEEDED);
  assert(strcmp(status(host, job_id).message, "components-ok") == 0);
  assert(h2_lua_job_release(host, job_id) == H2_PAL_OK);
  assert(atomic_load(&s_test_audio_close_count) == 2);
  assert(atomic_load(&s_test_audio_start_count) == 1);
  assert(atomic_load(&s_test_audio_stop_count) == 1);
  assert(atomic_load(&s_test_audio_mic_start_count) == 1);
  assert(atomic_load(&s_test_audio_mic_stop_count) == 1);
  /* The script wrote 1..6 then 7..10 to o1 (device frame = 4 bytes), 100..103
   * to o2, then closed both. The sub-frame tail of the first write must be
   * carried into the second one, so o1's bytes reach the device in order with
   * no silence spliced in between, and close must flush the zero-padded
   * remainder. */
  {
    static const uint8_t expected[] = {
        1, 2,   3,   4,           /* o1 write 1: first whole frame */
        5, 6,   7,   8,           /* o1 write 2: carried 5,6 + new 7,8 */
        100, 101, 102, 103,       /* o2 write: independent Track */
        9, 10,  0,   0,           /* o1 close: flush 9,10 zero padded */
    };
    assert(s_test_audio_frame_count == 4u);
    assert(s_test_audio_written_bytes == sizeof(expected));
    assert(memcmp(s_test_audio_written, expected, sizeof(expected)) == 0);
  }

  static const uint8_t optional_singletons_script[] =
      "local touch_ok=pcall(require,'lcd_touch');"
      "local audio_ok=pcall(require,'audio');"
      "return (not touch_ok and not audio_ok) and 'optional-ok' or 'bad'";
  const h2_pal_touch_api_t *touch = runtime->touch;
  const h2_pal_audio_api_t *audio = runtime->audio;
  runtime->touch = NULL;
  runtime->audio = NULL;
  assert(h2_lua_job_submit_text(host, NULL, "@optional-singletons.lua",
                                optional_singletons_script,
                                sizeof(optional_singletons_script) - 1u, NULL,
                                0u, &job_id) == H2_PAL_OK);
  run_until_terminal(host, job_id, 16u);
  assert(status(host, job_id).state == H2_LUA_JOB_SUCCEEDED);
  assert(strcmp(status(host, job_id).message, "optional-ok") == 0);
  assert(h2_lua_job_release(host, job_id) == H2_PAL_OK);
  runtime->touch = touch;
  runtime->audio = audio;

  static const uint8_t audio_wait_script[] =
      "local a=require('audio');local d=require('delay');"
      "assert(a.new_output({sample_rate=16000,channels=1,bits_per_sample=16}));"
      "d.delay_ms(500);return 'done'";
  h2_lua_job_id_t audio_job_1;
  h2_lua_job_id_t audio_job_2;
  atomic_store(&s_test_audio_close_count, 0);
  atomic_store(&s_test_audio_start_count, 0);
  atomic_store(&s_test_audio_stop_count, 0);
  assert(h2_lua_job_submit_text(host, NULL, "@audio-wait-1.lua",
                                audio_wait_script,
                                sizeof(audio_wait_script) - 1u, NULL, 0u,
                                &audio_job_1) == H2_PAL_OK);
  assert(h2_lua_job_submit_text(host, NULL, "@audio-wait-2.lua",
                                audio_wait_script,
                                sizeof(audio_wait_script) - 1u, NULL, 0u,
                                &audio_job_2) == H2_PAL_OK);
  while (status(host, audio_job_1).state != H2_LUA_JOB_WAITING ||
         status(host, audio_job_2).state != H2_LUA_JOB_WAITING) {
    assert(h2_lua_host_step(host) == H2_PAL_OK);
    assert(h2_pal_time_sleep_ms(runtime->time, 1u) == H2_PAL_OK);
  }
  assert(atomic_load(&s_test_audio_start_count) == 1);
  assert(h2_lua_job_cancel(host, audio_job_1) == H2_PAL_OK);
  run_until_terminal(host, audio_job_1, 16u);
  assert(h2_lua_job_release(host, audio_job_1) == H2_PAL_OK);
  assert(atomic_load(&s_test_audio_close_count) == 1);
  assert(atomic_load(&s_test_audio_stop_count) == 0);
  assert(h2_lua_job_cancel(host, audio_job_2) == H2_PAL_OK);
  run_until_terminal(host, audio_job_2, 16u);
  assert(h2_lua_job_release(host, audio_job_2) == H2_PAL_OK);
  assert(atomic_load(&s_test_audio_close_count) == 2);
  assert(atomic_load(&s_test_audio_stop_count) == 1);

  static const uint8_t audio_input_wait_script[] =
      "local a=require('audio');local d=require('delay');"
      "assert(a.new_input());d.delay_ms(500);return 'done'";
  h2_lua_job_id_t audio_input_job_1;
  h2_lua_job_id_t audio_input_job_2;
  atomic_store(&s_test_audio_mic_start_count, 0);
  atomic_store(&s_test_audio_mic_stop_count, 0);
  assert(h2_lua_job_submit_text(host, NULL, "@audio-input-wait-1.lua",
                                audio_input_wait_script,
                                sizeof(audio_input_wait_script) - 1u, NULL, 0u,
                                &audio_input_job_1) == H2_PAL_OK);
  assert(h2_lua_job_submit_text(host, NULL, "@audio-input-wait-2.lua",
                                audio_input_wait_script,
                                sizeof(audio_input_wait_script) - 1u, NULL, 0u,
                                &audio_input_job_2) == H2_PAL_OK);
  while (status(host, audio_input_job_1).state != H2_LUA_JOB_WAITING ||
         status(host, audio_input_job_2).state != H2_LUA_JOB_WAITING) {
    assert(h2_lua_host_step(host) == H2_PAL_OK);
    assert(h2_pal_time_sleep_ms(runtime->time, 1u) == H2_PAL_OK);
  }
  assert(atomic_load(&s_test_audio_mic_start_count) == 1);
  assert(h2_lua_job_cancel(host, audio_input_job_1) == H2_PAL_OK);
  run_until_terminal(host, audio_input_job_1, 16u);
  assert(h2_lua_job_release(host, audio_input_job_1) == H2_PAL_OK);
  assert(atomic_load(&s_test_audio_mic_stop_count) == 0);
  assert(h2_lua_job_cancel(host, audio_input_job_2) == H2_PAL_OK);
  run_until_terminal(host, audio_input_job_2, 16u);
  assert(h2_lua_job_release(host, audio_input_job_2) == H2_PAL_OK);
  assert(atomic_load(&s_test_audio_mic_stop_count) == 1);

  {
    /* A microphone that never produces a frame must not let a script's long
     * input:read() timeout wedge host shutdown: the worker holds the job
     * mutex for the whole call, so h2_lua_host_stop/join have to complete
     * quickly by cancelling the read, not by waiting out the timeout. */
    static const uint8_t audio_input_block_script[] =
        "local a=require('audio');local input=assert(a.new_input());"
        "local ok,err=input:read(60000);return tostring(ok)..'|'..tostring(err)";
    h2_lua_host_t *block_host = create_host(runtime);
    h2_lua_job_id_t block_job_id;
    const h2_pal_time_api_t *real_time = h2_desktop_platform_time_api();
    uint64_t stop_started_ms;
    uint64_t stop_elapsed_ms;
    uint64_t join_elapsed_ms;
    size_t step;

    atomic_store(&s_test_audio_mic_start_count, 0);
    atomic_store(&s_test_audio_mic_stop_count, 0);
    atomic_store(&s_test_audio_mic_block, 1);
    assert(h2_lua_job_submit_text(block_host, NULL, "@audio-input-block.lua",
                                  audio_input_block_script,
                                  sizeof(audio_input_block_script) - 1u, NULL,
                                  0u, &block_job_id) == H2_PAL_OK);
    /* The worker holds the slot mutex for the whole blocking read, so polling
     * status via h2_lua_job_get_status here would itself block on that same
     * mutex. Watch the mic-acquired counter instead: it flips before the
     * script's input:read() call, without needing the lock. */
    for (step = 0u; step < 500u; ++step) {
      if (atomic_load(&s_test_audio_mic_start_count) != 0) {
        break;
      }
      assert(h2_lua_host_step(block_host) == H2_PAL_OK);
      (void)h2_pal_time_sleep_ms(real_time, 1u);
    }
    assert(atomic_load(&s_test_audio_mic_start_count) == 1);

    (void)h2_pal_time_get_monotonic_ms(real_time, &stop_started_ms);
    assert(h2_lua_host_stop(block_host) == H2_PAL_OK);
    assert(h2_lua_host_join(block_host) == H2_PAL_OK);
    {
      uint64_t joined_ms;
      (void)h2_pal_time_get_monotonic_ms(real_time, &joined_ms);
      stop_elapsed_ms = h2_pal_time_elapsed_ms(stop_started_ms, joined_ms);
    }
    join_elapsed_ms = stop_elapsed_ms;
    /* The mic never yields a frame, so with a stuck read this would only
     * unblock after the script's own 60s timeout. Bound the assertion well
     * under that to prove the wait was actually cancelled, not merely fast
     * on this machine. */
    assert(join_elapsed_ms < 5000u);

    atomic_store(&s_test_audio_mic_block, 0);
    h2_lua_host_destroy(block_host);
    assert(atomic_load(&s_test_audio_mic_stop_count) == 1);
  }

  static const uint8_t audio_failure_script[] =
      "local a=require('audio');"
      "assert(a.new_output({sample_rate=16000,channels=1,bits_per_sample=16}));"
      "error('forced failure')";
  atomic_store(&s_test_audio_close_count, 0);
  atomic_store(&s_test_audio_start_count, 0);
  atomic_store(&s_test_audio_stop_count, 0);
  assert(h2_lua_job_submit_text(host, NULL, "@audio-failure.lua",
                                audio_failure_script,
                                sizeof(audio_failure_script) - 1u, NULL, 0u,
                                &job_id) == H2_PAL_OK);
  run_until_terminal(host, job_id, 16u);
  assert(status(host, job_id).state == H2_LUA_JOB_FAILED);
  assert(atomic_load(&s_test_audio_close_count) == 0);
  assert(h2_lua_job_release(host, job_id) == H2_PAL_OK);
  assert(atomic_load(&s_test_audio_close_count) == 1);
  assert(atomic_load(&s_test_audio_stop_count) == 1);

  {
    static const uint8_t draw_circle_script[] =
        "local d=require('display');d.present();"
        "d.draw_circle(3,3,2,'red');d.present();d.deinit();return 'ok'";
    static const expected_pixel_t expected[] = {
        {3, 1}, {2, 1}, {4, 1}, {1, 2}, {5, 2}, {1, 3},
        {5, 3}, {1, 4}, {5, 4}, {2, 5}, {3, 5}, {4, 5},
    };
    h2_lua_job_status_t display_status =
        run_display_script(host, "@display-draw-circle.lua", draw_circle_script,
                           sizeof(draw_circle_script) - 1u);
    assert(strcmp(display_status.message, "ok") == 0);
    assert(s_test_display_fixture.draw_count == 2u);
    assert(s_test_display_fixture.present_count == 2u);
    assert_draw_rect(0u, 0, 0, 8, 8);
    assert_draw_rect(1u, 1, 1, 5, 5);
    assert_only_pixels(0xf800u, expected,
                       sizeof(expected) / sizeof(expected[0]));
  }

  {
    static const uint8_t clipped_circle_script[] =
        "local d=require('display');d.present();"
        "d.draw_circle(0,0,2,'blue');d.present();d.deinit();return 'ok'";
    static const expected_pixel_t expected[] = {
        {2, 0},
        {2, 1},
        {0, 2},
        {1, 2},
    };
    (void)run_display_script(host, "@display-clipped-circle.lua",
                             clipped_circle_script,
                             sizeof(clipped_circle_script) - 1u);
    assert(s_test_display_fixture.draw_count == 2u);
    assert_draw_rect(1u, 0, 0, 3, 3);
    assert_only_pixels(0x001fu, expected,
                       sizeof(expected) / sizeof(expected[0]));
  }

  {
    static const uint8_t radius_zero_script[] =
        "local d=require('display');d.present();"
        "d.draw_circle(7,0,0,'white');d.present();d.deinit();return 'ok'";
    static const expected_pixel_t expected[] = {{7, 0}};
    (void)run_display_script(host, "@display-radius-zero.lua",
                             radius_zero_script,
                             sizeof(radius_zero_script) - 1u);
    assert_draw_rect(1u, 7, 0, 1, 1);
    assert_only_pixels(0xffffu, expected,
                       sizeof(expected) / sizeof(expected[0]));
  }

  {
    static const uint8_t fill_rect_script[] =
        "local d=require('display');d.present();"
        "d.fill_rect(-1,1,3,2,'red');d.present();d.deinit();return 'ok'";
    static const expected_pixel_t expected[] = {
        {0, 1},
        {1, 1},
        {0, 2},
        {1, 2},
    };
    (void)run_display_script(host, "@display-fill-rect.lua", fill_rect_script,
                             sizeof(fill_rect_script) - 1u);
    assert_draw_rect(1u, 0, 1, 2, 2);
    assert_only_pixels(0xf800u, expected,
                       sizeof(expected) / sizeof(expected[0]));
  }

  {
    static const uint8_t fill_circle_script[] =
        "local d=require('display');d.present();"
        "d.fill_circle(3,3,2,'green');d.present();d.deinit();return 'ok'";
    static const expected_pixel_t expected[] = {
        {3, 1}, {2, 2}, {3, 2}, {4, 2}, {1, 3}, {2, 3}, {3, 3},
        {4, 3}, {5, 3}, {2, 4}, {3, 4}, {4, 4}, {3, 5},
    };
    (void)run_display_script(host, "@display-fill-circle.lua",
                             fill_circle_script,
                             sizeof(fill_circle_script) - 1u);
    assert_draw_rect(1u, 1, 1, 5, 5);
    assert_only_pixels(0x0400u, expected,
                       sizeof(expected) / sizeof(expected[0]));
  }

  {
    static const uint8_t fill_round_rect_script[] =
        "local d=require('display');d.present();"
        "d.fill_round_rect(1,1,6,4,2,'blue');"
        "d.present();d.deinit();return 'ok'";
    static const expected_pixel_t expected[] = {
        {3, 1}, {4, 1}, {2, 2}, {3, 2}, {4, 2}, {5, 2},
        {2, 3}, {3, 3}, {4, 3}, {5, 3}, {3, 4}, {4, 4},
    };
    (void)run_display_script(host, "@display-fill-round-rect.lua",
                             fill_round_rect_script,
                             sizeof(fill_round_rect_script) - 1u);
    assert_draw_rect(1u, 1, 1, 6, 4);
    assert_only_pixels(0x001fu, expected,
                       sizeof(expected) / sizeof(expected[0]));
  }

  {
    static const uint8_t draw_round_rect_script[] =
        "local d=require('display');d.present();"
        "d.draw_round_rect(1,1,6,4,2,'white');"
        "d.present();d.deinit();return 'ok'";
    static const expected_pixel_t expected[] = {
        {3, 1}, {4, 1}, {2, 2}, {5, 2}, {2, 3}, {5, 3}, {3, 4}, {4, 4},
    };
    (void)run_display_script(host, "@display-draw-round-rect.lua",
                             draw_round_rect_script,
                             sizeof(draw_round_rect_script) - 1u);
    assert_draw_rect(1u, 1, 1, 6, 4);
    assert_only_pixels(0xffffu, expected,
                       sizeof(expected) / sizeof(expected[0]));
  }

  {
    static const uint8_t dirty_union_script[] =
        "local d=require('display');d.present();"
        "d.fill_rect(1,2,2,2,'red');d.draw_circle(5,4,1,'red');"
        "d.present();d.deinit();return 'ok'";
    (void)run_display_script(host, "@display-dirty-union.lua",
                             dirty_union_script,
                             sizeof(dirty_union_script) - 1u);
    assert_draw_rect(1u, 1, 2, 6, 4);
  }

  {
    static const uint8_t clear_script[] =
        "local d=require('display');d.present();d.clear('red');"
        "d.present();d.deinit();return 'ok'";
    static const expected_pixel_t expected[] = {
        {0, 0}, {1, 0}, {2, 0}, {3, 0}, {4, 0}, {5, 0}, {6, 0}, {7, 0},
        {0, 1}, {1, 1}, {2, 1}, {3, 1}, {4, 1}, {5, 1}, {6, 1}, {7, 1},
        {0, 2}, {1, 2}, {2, 2}, {3, 2}, {4, 2}, {5, 2}, {6, 2}, {7, 2},
        {0, 3}, {1, 3}, {2, 3}, {3, 3}, {4, 3}, {5, 3}, {6, 3}, {7, 3},
        {0, 4}, {1, 4}, {2, 4}, {3, 4}, {4, 4}, {5, 4}, {6, 4}, {7, 4},
        {0, 5}, {1, 5}, {2, 5}, {3, 5}, {4, 5}, {5, 5}, {6, 5}, {7, 5},
        {0, 6}, {1, 6}, {2, 6}, {3, 6}, {4, 6}, {5, 6}, {6, 6}, {7, 6},
        {0, 7}, {1, 7}, {2, 7}, {3, 7}, {4, 7}, {5, 7}, {6, 7}, {7, 7},
    };
    (void)run_display_script(host, "@display-clear.lua", clear_script,
                             sizeof(clear_script) - 1u);
    assert_draw_rect(1u, 0, 0, 8, 8);
    assert_only_pixels(0xf800u, expected,
                       sizeof(expected) / sizeof(expected[0]));
  }

  {
    static const uint8_t draw_circle_error_script[] =
        "local d=require('display');"
        "local a,ae=pcall(d.draw_circle,0,0,-1,'red');"
        "local b,be=pcall(d.draw_circle,0,0,9,'red');"
        "local c,ce=pcall(d.draw_circle,17,0,1,'red');"
        "d.deinit();assert(not a and not b and not c);"
        "assert(string.find(ae,'invalid draw_circle',1,true));"
        "assert(string.find(be,'invalid draw_circle',1,true));"
        "assert(string.find(ce,'invalid draw_circle',1,true));"
        "return 'draw-circle-errors'";
    h2_lua_job_status_t display_status = run_display_script(
        host, "@display-draw-circle-errors.lua", draw_circle_error_script,
        sizeof(draw_circle_error_script) - 1u);
    assert(strcmp(display_status.message, "draw-circle-errors") == 0);
  }

  static const uint8_t display_overflow_script[] =
      "local d=require('display');"
      "local line_ok,line_err=pcall(d.draw_line,-2147483648,0,2147483647,0,"
      "{r=0,g=0,b=0});"
      "local ok,err=pcall(d.draw_text_aligned,2147483647,0,2147483647,1,'x',"
      "{color={r=0,g=0,b=0},align='right'});"
      "d.deinit();return tostring(line_err)..':'..tostring(err)";
  assert(h2_lua_job_submit_text(host, NULL, "@display-overflow.lua",
                                display_overflow_script,
                                sizeof(display_overflow_script) - 1u, NULL, 0u,
                                &job_id) == H2_PAL_OK);
  run_until_terminal(host, job_id, 16u);
  assert(status(host, job_id).state == H2_LUA_JOB_SUCCEEDED);
  assert(strstr(status(host, job_id).message, "invalid draw_line") != NULL);
  if (strstr(status(host, job_id).message, "invalid draw_text_aligned") ==
      NULL) {
    fprintf(stderr, "display overflow output=%s\n",
            status(host, job_id).message);
  }
  assert(strstr(status(host, job_id).message, "invalid draw_text_aligned") !=
         NULL);
  assert(h2_lua_job_release(host, job_id) == H2_PAL_OK);

  static const uint8_t display_aa_script[] =
      "local d=require('display');"
      "d.begin_frame({clear=true,color='white'});"
      "d.fade_to_black(38);"
      "d.fade_rect_to_black(2,2,4,4,38);"
      "d.fill_circle_aa(4,4,2,'black');"
      "d.deinit();return 'display-aa-ok'";
  assert(h2_lua_job_submit_text(
             host, NULL, "@display-aa.lua", display_aa_script,
             sizeof(display_aa_script) - 1u, NULL, 0u, &job_id) == H2_PAL_OK);
  run_until_terminal(host, job_id, 16u);
  assert(status(host, job_id).state == H2_LUA_JOB_SUCCEEDED);
  assert(strcmp(status(host, job_id).message, "display-aa-ok") == 0);
  assert(h2_lua_job_release(host, job_id) == H2_PAL_OK);

  h2_lua_host_destroy(host);
  h2_runtime_deinit(runtime);

  static const uint8_t resume_budget_script[] =
      "local n=0;for i=1,20 do n=n+i end;return tostring(n)";
  test_clock_t clock;
  atomic_init(&clock.now_ms, 0u);
  const h2_pal_time_api_t test_time = {
      .user = &clock,
      .vtable = &s_test_clock_vtable,
  };
  runtime = create_runtime_with_time(&test_time);
  host = create_unstarted_host_with_scheduler(runtime, 1u, UINT32_MAX,
                                              UINT32_MAX, 4096u);
  assert(h2_lua_host_start(host) == H2_PAL_OK);
  assert(h2_lua_job_submit_text(host, NULL, "@large-budget.lua",
                                resume_budget_script,
                                sizeof(resume_budget_script) - 1u, NULL, 0u,
                                &job_id) == H2_PAL_OK);
  run_until_terminal(host, job_id, 16u);
  assert(status(host, job_id).state == H2_LUA_JOB_SUCCEEDED);
  assert(status(host, job_id).resume_count == 1u);
  assert(h2_lua_job_release(host, job_id) == H2_PAL_OK);
  h2_lua_host_destroy(host);

  atomic_store(&clock.now_ms, 0u);
  host =
      create_unstarted_host_with_scheduler(runtime, 1u, 1u, UINT32_MAX, 4096u);
  assert(h2_lua_host_start(host) == H2_PAL_OK);
  assert(h2_lua_job_submit_text(host, NULL, "@small-budget.lua",
                                resume_budget_script,
                                sizeof(resume_budget_script) - 1u, NULL, 0u,
                                &job_id) == H2_PAL_OK);
  run_until_terminal(host, job_id, 256u);
  assert(status(host, job_id).state == H2_LUA_JOB_SUCCEEDED);
  assert(status(host, job_id).resume_count > 1u);
  assert(h2_lua_job_release(host, job_id) == H2_PAL_OK);

  static const uint8_t yielded_arguments_script[] =
      "local function rgb(r,g,b)return{r=r,g=g,b=b}end;"
      "local function fill_rect(x,y,w,h,c)"
      "assert(type(x)=='number' and y==120 and w==47 and h==10 and "
      "c.r==1 and c.g==2 and c.b==3)end;"
      "local total=0;for i=1,32 do local x=i*3;local top_h=130;"
      "fill_rect(x-2,top_h-10,43+4,10,rgb(1,2,3));total=total+x end;"
      "return tostring(total)";
  assert(h2_lua_job_submit_text(host, NULL, "@yielded-arguments.lua",
                                yielded_arguments_script,
                                sizeof(yielded_arguments_script) - 1u, NULL, 0u,
                                &job_id) == H2_PAL_OK);
  run_until_terminal(host, job_id, 4096u);
  assert(status(host, job_id).state == H2_LUA_JOB_SUCCEEDED);
  assert(status(host, job_id).resume_count > 32u);
  assert(strcmp(status(host, job_id).message, "1584") == 0);
  assert(h2_lua_job_release(host, job_id) == H2_PAL_OK);
  h2_lua_host_destroy(host);
  h2_runtime_deinit(runtime);

  runtime = create_runtime();
  host = create_host(runtime);

  assert(h2_lua_esp_claw_module_count() == 36u);
  for (size_t i = 0u; i < 36u; ++i) {
    const h2_lua_esp_claw_module_info_t *module = h2_lua_esp_claw_module_at(i);
    assert(module != NULL);
    assert(strcmp(module->id, esp_claw_ids[i]) == 0);
    if (strcmp(module->id, "json") == 0 ||
        strcmp(module->id, "capability") == 0) {
      assert(module->status == H2_LUA_ESP_CLAW_MODULE_FULL);
    } else if (strcmp(module->id, "delay") == 0 ||
               strcmp(module->id, "system") == 0 ||
               strcmp(module->id, "display") == 0 ||
               strcmp(module->id, "lcd_touch") == 0 ||
               strcmp(module->id, "audio") == 0 ||
               strcmp(module->id, "storage") == 0) {
      assert(module->status == H2_LUA_ESP_CLAW_MODULE_PROFILE);
    } else if (strcmp(module->id, "button") == 0) {
      assert(module->status == H2_LUA_ESP_CLAW_MODULE_COMPONENT_ADAPTED);
    } else {
      assert(module->status == H2_LUA_ESP_CLAW_MODULE_UNAVAILABLE);
    }
  }
  assert(h2_lua_esp_claw_module_at(36u) == NULL);
  assert(h2_lua_extension_count() == 1u);
  for (size_t i = 0u; i < 1u; ++i) {
    assert(strcmp(h2_lua_extension_at(i), extension_ids[i]) == 0);
  }
  assert(h2_lua_extension_at(1u) == NULL);
  assert(h2_lua_job_submit_text(host, NULL, "@test.lua", script,
                                sizeof(script) - 1u, args, 1u,
                                &job_id) == H2_PAL_OK);
  while (status(host, job_id).state != H2_LUA_JOB_WAITING) {
    assert(h2_lua_host_step(host) == H2_PAL_OK);
    assert(h2_pal_time_sleep_ms(runtime->time, 1u) == H2_PAL_OK);
  }
  click_event.payload_size--;
  assert(h2_lua_dispatch_runtime_event(host, job_id, &click_event) ==
         H2_PAL_ERR_INVALID_ARG);
  click_event.payload_size++;
  click_event.component = H2_RUNTIME_COMPONENT_IMU;
  assert(h2_lua_dispatch_runtime_event(host, job_id, &click_event) ==
         H2_PAL_ERR_INVALID_ARG);
  click_event.component = H2_RUNTIME_COMPONENT_BUTTON;
  click_event.component_id = 999u;
  assert(h2_lua_dispatch_runtime_event(host, job_id, &click_event) ==
         H2_PAL_ERR_NOT_FOUND);
  click_event.component_id = 7u;
  click.pressed_at_ms = 3u;
  assert(h2_lua_dispatch_runtime_event(host, job_id, &click_event) ==
         H2_PAL_ERR_INVALID_ARG);
  click.pressed_at_ms = 1u;
  click.released_at_ms = 0u;
  assert(h2_lua_dispatch_runtime_event(host, job_id, &click_event) ==
         H2_PAL_OK);
  click.released_at_ms = 3u;
  assert(h2_lua_dispatch_runtime_event(host, job_id, &click_event) ==
         H2_PAL_ERR_INVALID_ARG);
  click.released_at_ms = 2u;
  assert(h2_lua_dispatch_runtime_event(host, job_id, &click_event) ==
         H2_PAL_OK);
  click = (h2_runtime_button_action_event_t){
      .pressed_at_ms = 10u,
      .released_at_ms = 0u,
  };
  click_event.timestamp_ms = 10u;
  assert(h2_lua_dispatch_runtime_event(host, job_id, &click_event) ==
         H2_PAL_OK);
  click_event.timestamp_ms = 510u;
  assert(h2_lua_dispatch_runtime_event(host, job_id, &click_event) ==
         H2_PAL_OK);
  click.released_at_ms = 610u;
  click_event.timestamp_ms = 610u;
  assert(h2_lua_dispatch_runtime_event(host, job_id, &click_event) ==
         H2_PAL_OK);
  assert(h2_lua_dispatch_runtime_event(host, job_id, &down_event) == H2_PAL_OK);
  assert(h2_lua_dispatch_runtime_event(host, job_id, &up_event) == H2_PAL_OK);
  assert(h2_lua_dispatch_runtime_event(host, job_id, &nfc_event) == H2_PAL_OK);
  assert(h2_lua_dispatch_runtime_event(host, job_id, &imu_event) == H2_PAL_OK);
  imu_event.payload = &imu_tilt;
  assert(h2_lua_dispatch_runtime_event(host, job_id, &imu_event) == H2_PAL_OK);
  imu_event.payload = &imu_flip;
  assert(h2_lua_dispatch_runtime_event(host, job_id, &imu_event) == H2_PAL_OK);
  imu_event.payload = &imu_fall;
  assert(h2_lua_dispatch_runtime_event(host, job_id, &imu_event) == H2_PAL_OK);
  assert(h2_lua_dispatch_runtime_event(host, job_id, &error_event) ==
         H2_PAL_OK);
  run_until_terminal(host, job_id, 32u);
  if (status(host, job_id).state != H2_LUA_JOB_SUCCEEDED) {
    h2_lua_job_status_t event_status = status(host, job_id);
    fprintf(stderr, "event state=%d message=%s resumes=%llu\n",
            event_status.state, event_status.message,
            (unsigned long long)event_status.resume_count);
  }
  assert(status(host, job_id).state == H2_LUA_JOB_SUCCEEDED);
  assert(strcmp(status(host, job_id).message, "ready:4095") == 0);
  assert(h2_lua_dispatch_runtime_event(host, job_id, &click_event) ==
         H2_PAL_ERR_CLOSED);
  assert(h2_lua_job_release(host, job_id) == H2_PAL_OK);

  static const uint8_t nested_cpu_script[] =
      "local c=coroutine.create(function() while true do end end);"
      "local ok=coroutine.resume(c);"
      "return ok and coroutine.status(c) or 'failed'";
  assert(h2_lua_job_submit_text(
             host, NULL, "@nested-cpu.lua", nested_cpu_script,
             sizeof(nested_cpu_script) - 1u, NULL, 0u, &job_id) == H2_PAL_OK);
  run_until_terminal(host, job_id, 16u);
  assert(status(host, job_id).state == H2_LUA_JOB_SUCCEEDED);
  assert(strcmp(status(host, job_id).message, "suspended") == 0);
  assert(h2_lua_job_release(host, job_id) == H2_PAL_OK);
  h2_lua_host_destroy(host);
  h2_runtime_deinit(runtime);

  runtime = create_runtime();
  host = create_host(runtime);
  static const uint8_t async_script[] =
      "local a=require('runtime');local trace={}\n"
      "local x=a.spawn(function(v) "
      "trace[#trace+1]='a';a.yield();trace[#trace+1]='A';return v end,'x')\n"
      "local y=a.spawn(function(v) "
      "trace[#trace+1]='b';a.yield();trace[#trace+1]='B';return v end,'y')\n"
      "local okx,rx=a.join(x);local oky,ry=a.join(y)\n"
      "return table.concat(trace)..':'..tostring(okx)..rx..tostring(oky)..ry\n";
  assert(h2_lua_job_submit_text(host, NULL, "@async.lua", async_script,
                                sizeof(async_script) - 1u, NULL, 0u,
                                &job_id) == H2_PAL_OK);
  run_until_terminal(host, job_id, 32u);
  assert(status(host, job_id).state == H2_LUA_JOB_SUCCEEDED);
  if (strcmp(status(host, job_id).message, "abAB:truextruey") != 0) {
    fprintf(stderr, "async output=%s\n", status(host, job_id).message);
  }
  assert(strcmp(status(host, job_id).message, "abAB:truextruey") == 0);
  assert(h2_lua_job_release(host, job_id) == H2_PAL_OK);

  static const uint8_t failed_child_script[] =
      "local a=require('runtime');"
      "local child=a.spawn(function()error('child failed')end);"
      "local ok,message=a.join(child);"
      "return tostring(ok)..':'..tostring(message):match('child failed')";
  assert(h2_lua_job_submit_text(
             host, NULL, "@failed-child.lua", failed_child_script,
             sizeof(failed_child_script) - 1u, NULL, 0u, &job_id) == H2_PAL_OK);
  run_until_terminal(host, job_id, 32u);
  assert(status(host, job_id).state == H2_LUA_JOB_SUCCEEDED);
  assert(strcmp(status(host, job_id).message, "false:child failed") == 0);
  assert(h2_lua_job_release(host, job_id) == H2_PAL_OK);
  h2_lua_host_destroy(host);
  h2_runtime_deinit(runtime);

  runtime = create_runtime();
  host = create_unstarted_host(runtime);
  capability_fixture_t capability = {.host = host};
  assert(h2_lua_register_capability(host, "immediate", immediate_capability,
                                    NULL, NULL) == H2_PAL_OK);
  assert(h2_lua_register_capability(host, "early",
                                    completed_before_return_capability, NULL,
                                    &capability) == H2_PAL_OK);
  assert(h2_lua_register_capability(host, "pending", pending_capability,
                                    cancel_capability,
                                    &capability) == H2_PAL_OK);
  assert(h2_lua_host_start(host) == H2_PAL_OK);
  static const uint8_t capability_script[] =
      "local c=require('capability')\n"
      "local a,b,e=c.call('immediate',{a=1},{z=true})\n"
      "local x,y,z=c.call('early',nil,nil)\n"
      "return "
      "tostring(a)..':'..tostring(b)..':'..tostring(e)..':'..tostring(x)..':'.."
      "tostring(y)..':'.."
      "tostring(z)\n";
  assert(h2_lua_job_submit_text(
             host, NULL, "@capability.lua", capability_script,
             sizeof(capability_script) - 1u, NULL, 0u, &job_id) == H2_PAL_OK);
  run_until_terminal(host, job_id, 16u);
  h2_lua_job_status_t capability_status = status(host, job_id);
  if (capability_status.state != H2_LUA_JOB_SUCCEEDED) {
    fprintf(stderr, "capability state=%d message=%s resumes=%llu\n",
            capability_status.state, capability_status.message,
            (unsigned long long)capability_status.resume_count);
  }
  assert(capability_status.state == H2_LUA_JOB_SUCCEEDED);
  if (strcmp(capability_status.message, "true:ok:nil:true:async:nil") != 0) {
    fprintf(stderr, "capability output=%s\n", capability_status.message);
  }
  assert(strcmp(capability_status.message, "true:ok:nil:true:async:nil") == 0);
  assert(h2_lua_job_release(host, job_id) == H2_PAL_OK);

  static const uint8_t pending_script[] =
      "local c=require('capability');return c.call('pending',nil,nil)";
  assert(h2_lua_job_submit_text(host, NULL, "@pending.lua", pending_script,
                                sizeof(pending_script) - 1u, NULL, 0u,
                                &job_id) == H2_PAL_OK);
  capability.job_id = job_id;
  while (status(host, job_id).state != H2_LUA_JOB_WAITING) {
    assert(h2_lua_host_step(host) == H2_PAL_OK);
    assert(h2_pal_time_sleep_ms(runtime->time, 1u) == H2_PAL_OK);
  }
  assert(h2_lua_job_cancel(host, job_id) == H2_PAL_OK);
  assert(capability.cancel_count == 1u);
  assert(capability.cancel_status_result == H2_PAL_OK);
  assert(h2_lua_capability_complete(host, capability.pending_id, H2_PAL_OK,
                                    "late", NULL) == H2_PAL_ERR_CLOSED);
  run_until_terminal(host, job_id, 16u);
  assert(status(host, job_id).state == H2_LUA_JOB_CANCELLED);
  assert(h2_lua_job_release(host, job_id) == H2_PAL_OK);
  capability.pending_id = 0u;
  assert(h2_lua_job_submit_text(host, NULL, "@pending-again.lua",
                                pending_script, sizeof(pending_script) - 1u,
                                NULL, 0u, &job_id) == H2_PAL_OK);
  capability.job_id = job_id;
  while (status(host, job_id).state != H2_LUA_JOB_WAITING) {
    assert(h2_lua_host_step(host) == H2_PAL_OK);
    assert(h2_pal_time_sleep_ms(runtime->time, 1u) == H2_PAL_OK);
  }
  assert(h2_lua_job_cancel(host, job_id) == H2_PAL_OK);
  assert(capability.cancel_count == 2u);
  assert(h2_lua_capability_complete(host, capability.pending_id, H2_PAL_OK,
                                    "late", NULL) == H2_PAL_ERR_CLOSED);
  run_until_terminal(host, job_id, 16u);
  assert(h2_lua_job_release(host, job_id) == H2_PAL_OK);
  h2_lua_host_destroy(host);
  h2_runtime_deinit(runtime);
  return 0;
}
