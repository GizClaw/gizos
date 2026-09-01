#include "h2_desktop_platform.h"
#include "h2_lua.h"
#include "h2_lua_capability.h"
#include "h2_lua_esp_claw.h"
#include "h2_lua_event.h"
#include "h2_lua_job.h"
#include "h2_pal.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
  uint16_t pixels[8u * 8u];
  h2_display_rect_t draw_rects[8u];
  size_t draw_count;
  size_t present_count;
} test_display_fixture_t;

static test_display_fixture_t s_test_display_fixture;

static void test_display_reset(void) {
  memset(&s_test_display_fixture, 0, sizeof(s_test_display_fixture));
}

static int test_display_open(void *user) {
  assert(user == &s_test_display_fixture);
  return H2_DISPLAY_OK;
}

static int test_display_get_info(void *user, h2_display_info_t *info) {
  assert(user == &s_test_display_fixture);
  if (info == NULL)
    return H2_DISPLAY_ERR_INVALID_ARG;
  *info = (h2_display_info_t){
      .width = 8,
      .height = 8,
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
         rect->x + rect->width <= 8 && rect->y + rect->height <= 8);
  assert(stride_bytes >= (size_t)rect->width * sizeof(uint16_t));
  assert(fixture->draw_count <
         sizeof(fixture->draw_rects) / sizeof(fixture->draw_rects[0]));
  fixture->draw_rects[fixture->draw_count++] = *rect;
  for (row = 0; row < rect->height; ++row) {
    memcpy(fixture->pixels + (size_t)(rect->y + row) * 8u + (size_t)rect->x,
           source + (size_t)row * stride_bytes,
           (size_t)rect->width * sizeof(uint16_t));
  }
  return H2_DISPLAY_OK;
}

static int test_display_present(void *user) {
  test_display_fixture_t *fixture = user;
  assert(fixture != NULL);
  fixture->present_count++;
  return H2_DISPLAY_OK;
}

static int test_display_close(void *user) {
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
      .playback_supported = 1,
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
    .start_speaker = test_audio_start_speaker,
    .stop_speaker = test_audio_stop_speaker,
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

static void assert_missing_worker_apis_are_rejected(h2_runtime_t *runtime) {
  const h2_pal_queue_api_t *queue = runtime->queue;
  const h2_pal_task_api_t *task = runtime->task;
  h2_pal_queue_vtable_t fallback_vtable = *queue->vtable;
  h2_pal_queue_api_t fallback_queue = {
      .user = queue->user,
      .vtable = &fallback_vtable,
  };
  h2_lua_host_t *host = NULL;
  h2_lua_host_config_t config = {.runtime = runtime};
  runtime->queue = NULL;
  assert(h2_lua_host_create(&config, &host) == H2_PAL_ERR_UNSUPPORTED);
  assert(host == NULL);
  runtime->queue = queue;
  runtime->task = NULL;
  assert(h2_lua_host_create(&config, &host) == H2_PAL_ERR_UNSUPPORTED);
  assert(host == NULL);
  runtime->task = task;

  fallback_vtable.send_latest = NULL;
  runtime->queue = &fallback_queue;
  assert(h2_lua_host_create(&config, &host) == H2_PAL_OK);
  assert(h2_lua_host_start(host) == H2_PAL_OK);
  assert(h2_lua_host_step(host) == H2_PAL_OK);
  h2_lua_host_destroy(host);
  runtime->queue = queue;
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

static h2_lua_job_status_t run_display_script(h2_lua_host_t *host,
                                              const char *name,
                                              const uint8_t *script,
                                              size_t script_size) {
  h2_lua_job_id_t job_id;
  h2_lua_job_status_t job_status;
  test_display_reset();
  assert(h2_lua_job_submit_text(host, name, script, script_size, NULL, 0u,
                                &job_id) == H2_PAL_OK);
  run_until_terminal(host, job_id, 64u);
  job_status = status(host, job_id);
  assert(job_status.state == H2_LUA_JOB_SUCCEEDED);
  assert(h2_lua_job_release(host, job_id) == H2_PAL_OK);
  return job_status;
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

int main(void) {
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
             invalid_size_host, "@scripts/invalid-size-test.lua",
             invalid_size_script, sizeof(invalid_size_script) - 1u, NULL, 0u,
             &invalid_size_job_id) == H2_PAL_OK);
  run_until_terminal(invalid_size_host, invalid_size_job_id, 16u);
  assert(status(invalid_size_host, invalid_size_job_id).state ==
         H2_LUA_JOB_FAILED);
  assert(strstr(status(invalid_size_host, invalid_size_job_id).message,
                "source size is invalid") != NULL);
  assert(h2_lua_job_release(invalid_size_host, invalid_size_job_id) ==
         H2_PAL_OK);
  h2_lua_host_destroy(invalid_size_host);
  h2_lua_host_t *host = create_host(runtime);
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

  assert(h2_lua_job_submit_text(host, "@embedded-nul.lua", embedded_nul_source,
                                sizeof(embedded_nul_source), NULL, 0u,
                                &job_id) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_lua_job_submit_file(host, "../escape.lua", NULL, 0u, &job_id) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_lua_job_submit_file(host, "/absolute.lua", NULL, 0u, &job_id) ==
         H2_PAL_ERR_INVALID_ARG);
  const h2_pal_fs_api_t *fs = runtime->fs;
  runtime->fs = NULL;
  assert(h2_lua_job_submit_file(host, "scripts/main.lua", NULL, 0u, &job_id) ==
         H2_PAL_ERR_UNSUPPORTED);
  runtime->fs = fs;
  assert(h2_lua_job_submit_file(host, "scripts/oversize.lua", NULL, 0u,
                                &job_id) == H2_PAL_ERR_NO_SPACE);
  assert(h2_lua_job_submit_file(host, "scripts/bytecode.lua", NULL, 0u,
                                &job_id) == H2_PAL_ERR_FORMAT);
  assert(h2_lua_job_submit_file(host, "scripts/malformed.lua", NULL, 0u,
                                &job_id) == H2_PAL_OK);
  assert(status(host, job_id).state == H2_LUA_JOB_FAILED);
  assert(h2_lua_job_release(host, job_id) == H2_PAL_OK);
  assert(h2_lua_job_submit_file(host, "scripts/main.lua", file_args, 1u,
                                &job_id) == H2_PAL_OK);
  run_until_terminal(host, job_id, 16u);
  assert(status(host, job_id).state == H2_LUA_JOB_SUCCEEDED);
  assert(strcmp(status(host, job_id).message, "file:ok") == 0);
  assert(h2_lua_job_release(host, job_id) == H2_PAL_OK);

  assert(h2_lua_job_submit_text(host, "@system-profile.lua",
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
  s_test_audio_written_bytes = 0u;
  s_test_audio_frame_count = 0u;
  assert(h2_lua_job_submit_text(host, "@component-profile.lua",
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
  assert(h2_lua_job_submit_text(host, "@optional-singletons.lua",
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
  assert(h2_lua_job_submit_text(host, "@audio-wait-1.lua", audio_wait_script,
                                sizeof(audio_wait_script) - 1u, NULL, 0u,
                                &audio_job_1) == H2_PAL_OK);
  assert(h2_lua_job_submit_text(host, "@audio-wait-2.lua", audio_wait_script,
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

  static const uint8_t audio_failure_script[] =
      "local a=require('audio');"
      "assert(a.new_output({sample_rate=16000,channels=1,bits_per_sample=16}));"
      "error('forced failure')";
  atomic_store(&s_test_audio_close_count, 0);
  atomic_store(&s_test_audio_start_count, 0);
  atomic_store(&s_test_audio_stop_count, 0);
  assert(h2_lua_job_submit_text(host, "@audio-failure.lua",
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
  assert(h2_lua_job_submit_text(host, "@display-overflow.lua",
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
  assert(h2_lua_job_submit_text(host, "@large-budget.lua", resume_budget_script,
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
  assert(h2_lua_job_submit_text(host, "@small-budget.lua", resume_budget_script,
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
  assert(h2_lua_job_submit_text(host, "@yielded-arguments.lua",
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
               strcmp(module->id, "audio") == 0) {
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
  assert(h2_lua_job_submit_text(host, "@test.lua", script, sizeof(script) - 1u,
                                args, 1u, &job_id) == H2_PAL_OK);
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
  assert(h2_lua_job_submit_text(host, "@nested-cpu.lua", nested_cpu_script,
                                sizeof(nested_cpu_script) - 1u, NULL, 0u,
                                &job_id) == H2_PAL_OK);
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
  assert(h2_lua_job_submit_text(host, "@async.lua", async_script,
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
  assert(h2_lua_job_submit_text(host, "@failed-child.lua", failed_child_script,
                                sizeof(failed_child_script) - 1u, NULL, 0u,
                                &job_id) == H2_PAL_OK);
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
  assert(h2_lua_job_submit_text(host, "@capability.lua", capability_script,
                                sizeof(capability_script) - 1u, NULL, 0u,
                                &job_id) == H2_PAL_OK);
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
  assert(h2_lua_job_submit_text(host, "@pending.lua", pending_script,
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
  assert(h2_lua_job_submit_text(host, "@pending-again.lua", pending_script,
                                sizeof(pending_script) - 1u, NULL, 0u,
                                &job_id) == H2_PAL_OK);
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
