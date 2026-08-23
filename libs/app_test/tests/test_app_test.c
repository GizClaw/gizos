#include "h2_app_test.h"
#include "h2_app_test_case.h"
#include "h2_app_test_memory.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(h2_app_test_snapshot_t) <= 16u * 1024u,
               "snapshot must remain practical for a device case stack");

struct h2_pal_queue {
  size_t item_size;
  size_t item_count;
  size_t count;
  size_t head;
  size_t tail;
  uint8_t *items;
  const h2_pal_mem_api_t *allocator;
};

struct h2_pal_mutex {
  bool locked;
};

struct h2_pal_cond {
  bool active;
};

struct h2_pal_task {
  h2_pal_task_entry_t entry;
  void *ctx;
};

typedef struct fake_state {
  int32_t value;
} fake_state_t;

typedef struct fake_app {
  h2_runtime_t *runtime;
  int32_t value;
  h2_runtime_event_kind_t last_event_kind;
  uint16_t last_click_count;
  bool button_pressed;
  uint64_t button_updated_at_ms;
  unsigned int reset_count;
  unsigned int stop_count;
  bool fail_reset;
  bool fail_snapshot;
  unsigned int corrupt_snapshot;
  h2_pal_result_t step_result;
} fake_app_t;

static void *fake_alloc(void *user, size_t size) {
  (void)user;
  return malloc(size);
}

static void *fake_realloc(void *user, void *pointer, size_t size) {
  (void)user;
  return realloc(pointer, size);
}

static void fake_free(void *user, void *pointer) {
  (void)user;
  free(pointer);
}

static h2_pal_result_t fake_time_now(void *user, uint64_t *out_ms) {
  (void)user;
  if (out_ms == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_ms = 0u;
  return H2_PAL_OK;
}

static h2_pal_result_t fake_time_sleep(void *user, uint32_t ms) {
  (void)user;
  (void)ms;
  return H2_PAL_OK;
}

static int fake_task_start(
    void *user, const h2_pal_task_options_t *options,
    h2_pal_task_entry_t entry, void *ctx, h2_pal_task_t **out_task) {
  (void)user;
  (void)options;
  if (entry == NULL || out_task == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_pal_task_t *task = calloc(1u, sizeof(*task));
  if (task == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  task->entry = entry;
  task->ctx = ctx;
  *out_task = task;
  return H2_PAL_OK;
}

static int fake_task_join(void *user, h2_pal_task_t *task) {
  (void)user;
  if (task == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  free(task);
  return H2_PAL_OK;
}

static h2_pal_result_t fake_mutex_create(
    void *user, const h2_pal_mutex_config_t *config,
    h2_pal_mutex_t **out_mutex) {
  (void)user;
  if (config == NULL || out_mutex == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_mutex = calloc(1u, sizeof(**out_mutex));
  return *out_mutex != NULL ? H2_PAL_OK : H2_PAL_ERR_NO_MEMORY;
}

static h2_pal_result_t fake_mutex_destroy(
    void *user, h2_pal_mutex_t *mutex) {
  (void)user;
  if (mutex == NULL || mutex->locked) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  free(mutex);
  return H2_PAL_OK;
}

static h2_pal_result_t fake_mutex_lock(
    void *user, h2_pal_mutex_t *mutex) {
  (void)user;
  if (mutex == NULL || mutex->locked) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  mutex->locked = true;
  return H2_PAL_OK;
}

static h2_pal_result_t fake_mutex_unlock(
    void *user, h2_pal_mutex_t *mutex) {
  (void)user;
  if (mutex == NULL || !mutex->locked) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  mutex->locked = false;
  return H2_PAL_OK;
}

static h2_pal_result_t fake_cond_create(
    void *user, const h2_pal_cond_config_t *config,
    h2_pal_cond_t **out_cond) {
  (void)user;
  if (config == NULL || out_cond == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_cond = calloc(1u, sizeof(**out_cond));
  if (*out_cond == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  (*out_cond)->active = true;
  return H2_PAL_OK;
}

static h2_pal_result_t fake_cond_destroy(
    void *user, h2_pal_cond_t *cond) {
  (void)user;
  if (cond == NULL || !cond->active) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  cond->active = false;
  free(cond);
  return H2_PAL_OK;
}

static h2_pal_result_t fake_cond_wait(
    void *user, h2_pal_cond_t *cond, h2_pal_mutex_t *mutex,
    uint32_t timeout_ms) {
  (void)user;
  (void)timeout_ms;
  if (cond == NULL || !cond->active || mutex == NULL ||
      !mutex->locked) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  return H2_PAL_ERR_WOULD_BLOCK;
}

static h2_pal_result_t fake_cond_signal(
    void *user, h2_pal_cond_t *cond) {
  (void)user;
  return cond != NULL && cond->active ? H2_PAL_OK
                                      : H2_PAL_ERR_INVALID_ARG;
}

static h2_pal_result_t fake_periph_list(
    void *user, h2_pal_periph_type_t type_filter,
    h2_pal_periph_cb_t cb, void *cb_user) {
  (void)user;
  if (cb == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (type_filter != H2_PAL_PERIPH_TYPE_ANY &&
      type_filter != H2_PAL_PERIPH_TYPE_SINGLE_BUTTON) {
    return H2_PAL_OK;
  }
  static const h2_pal_periph_single_button_payload_t payload = {
      .delivery = H2_PAL_BUTTON_DELIVERY_PUSH_EDGE,
  };
  const h2_pal_periph_info_t info = {
      .id = 7u,
      .type = H2_PAL_PERIPH_TYPE_SINGLE_BUTTON,
      .name = "fake-button",
      .payload = &payload,
      .payload_size = sizeof(payload),
  };
  return cb(cb_user, &info);
}

static h2_pal_result_t fake_periph_get(
    void *user, h2_pal_periph_id_t id,
    h2_pal_periph_info_t *out_info) {
  (void)user;
  if (id != 7u || out_info == NULL) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  static const h2_pal_periph_single_button_payload_t payload = {
      .delivery = H2_PAL_BUTTON_DELIVERY_PUSH_EDGE,
  };
  *out_info = (h2_pal_periph_info_t){
      .id = 7u,
      .type = H2_PAL_PERIPH_TYPE_SINGLE_BUTTON,
      .name = "fake-button",
      .payload = &payload,
      .payload_size = sizeof(payload),
  };
  return H2_PAL_OK;
}

static h2_pal_result_t fake_component_list(
    void *user, h2_runtime_component_t component_filter,
    h2_runtime_component_mapping_cb_t cb, void *cb_user) {
  (void)user;
  if (cb == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (component_filter != H2_RUNTIME_COMPONENT_NONE &&
      component_filter != H2_RUNTIME_COMPONENT_BUTTON) {
    return H2_PAL_OK;
  }
  const h2_runtime_component_mapping_entry_t entry = {
      .component_id = 7u,
      .periph_id = 7u,
  };
  return cb(cb_user, &entry);
}

static int fake_queue_create(
    void *user, const h2_pal_queue_config_t *config,
    h2_pal_queue_t **out_queue) {
  (void)user;
  if (config == NULL || config->allocator == NULL || out_queue == NULL ||
      config->item_size == 0u || config->item_count == 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_queue = NULL;
  h2_pal_queue_t *queue =
      h2_pal_mem_alloc(config->allocator, sizeof(*queue));
  if (queue == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  memset(queue, 0, sizeof(*queue));
  queue->items = h2_pal_mem_alloc(
      config->allocator, config->item_size * config->item_count);
  if (queue->items == NULL) {
    h2_pal_mem_free(config->allocator, queue);
    return H2_PAL_ERR_NO_MEMORY;
  }
  queue->item_size = config->item_size;
  queue->item_count = config->item_count;
  queue->allocator = config->allocator;
  *out_queue = queue;
  return H2_PAL_OK;
}

static void fake_queue_destroy(void *user, h2_pal_queue_t *queue) {
  (void)user;
  if (queue == NULL) {
    return;
  }
  h2_pal_mem_free(queue->allocator, queue->items);
  h2_pal_mem_free(queue->allocator, queue);
}

static int fake_queue_send(
    void *user, h2_pal_queue_t *queue, const void *item,
    uint32_t timeout_ms) {
  (void)user;
  (void)timeout_ms;
  if (queue == NULL || item == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (queue->count == queue->item_count) {
    return H2_PAL_ERR_FULL;
  }
  memcpy(
      &queue->items[queue->tail * queue->item_size],
      item,
      queue->item_size);
  queue->tail = (queue->tail + 1u) % queue->item_count;
  ++queue->count;
  return H2_PAL_OK;
}

static int fake_queue_recv(
    void *user, h2_pal_queue_t *queue, void *out_item,
    uint32_t timeout_ms) {
  (void)user;
  if (queue == NULL || out_item == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (queue->count == 0u) {
    return timeout_ms == 0u ? H2_PAL_ERR_WOULD_BLOCK : H2_PAL_ERR_TIMEOUT;
  }
  memcpy(
      out_item,
      &queue->items[queue->head * queue->item_size],
      queue->item_size);
  queue->head = (queue->head + 1u) % queue->item_count;
  --queue->count;
  return H2_PAL_OK;
}

static int fake_queue_reset(
    void *user, h2_pal_queue_t *queue) {
  (void)user;
  if (queue == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  queue->count = 0u;
  queue->head = 0u;
  queue->tail = 0u;
  return H2_PAL_OK;
}

static const h2_pal_mem_vtable_t s_mem_vtable = {
    .alloc = fake_alloc,
    .realloc = fake_realloc,
    .free = fake_free,
};

static const h2_pal_mem_api_t s_mem = {
    .user = NULL,
    .vtable = &s_mem_vtable,
};

static const h2_pal_time_vtable_t s_time_vtable = {
    .get_monotonic_ms = fake_time_now,
    .sleep_ms = fake_time_sleep,
};

static const h2_pal_time_api_t s_time = {
    .user = NULL,
    .vtable = &s_time_vtable,
};

static const h2_pal_task_vtable_t s_task_vtable = {
    .start = fake_task_start,
    .join = fake_task_join,
};

static const h2_pal_task_api_t s_task = {
    .user = NULL,
    .vtable = &s_task_vtable,
};

static const h2_pal_queue_vtable_t s_queue_vtable = {
    .create = fake_queue_create,
    .destroy = fake_queue_destroy,
    .send = fake_queue_send,
    .recv = fake_queue_recv,
    .reset = fake_queue_reset,
};

static const h2_pal_queue_api_t s_queue = {
    .user = NULL,
    .vtable = &s_queue_vtable,
};

static const h2_pal_sync_vtable_t s_sync_vtable = {
    .create_mutex = fake_mutex_create,
    .destroy_mutex = fake_mutex_destroy,
    .lock_mutex = fake_mutex_lock,
    .try_lock_mutex = fake_mutex_lock,
    .unlock_mutex = fake_mutex_unlock,
    .create_cond = fake_cond_create,
    .destroy_cond = fake_cond_destroy,
    .wait_cond = fake_cond_wait,
    .signal_cond = fake_cond_signal,
    .broadcast_cond = fake_cond_signal,
};

static const h2_pal_sync_api_t s_sync = {
    .user = NULL,
    .vtable = &s_sync_vtable,
};

static const h2_pal_periph_vtable_t s_periph_vtable = {
    .list = fake_periph_list,
    .get = fake_periph_get,
};

static const h2_pal_periph_api_t s_periph = {
    .user = NULL,
    .vtable = &s_periph_vtable,
};

static const h2_runtime_component_mapper_vtable_t s_mapper_vtable = {
    .list = fake_component_list,
};

static const h2_runtime_component_mapper_t s_mapper = {
    .user = NULL,
    .vtable = &s_mapper_vtable,
};

static h2_runtime_config_t fake_runtime_config(void) {
  return (h2_runtime_config_t){
      .board = "app-test",
      .target = "memory",
      .chip = "host",
      .firmware_info = h2_pal_unsupported_firmware_info_api(),
      .mem = &s_mem,
      .log = h2_pal_unsupported_log_api(),
      .time = &s_time,
      .timer = h2_pal_unsupported_timer_api(),
      .task = &s_task,
      .queue = &s_queue,
      .sync = &s_sync,
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
      .periph = &s_periph,
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
      .component_mapper = &s_mapper,
      .event_queue_capacity = 4u,
  };
}

static h2_pal_result_t fake_reset(
    void *user, const h2_app_test_fixture_t *fixture) {
  fake_app_t *app = user;
  ++app->reset_count;
  if (app->fail_reset) {
    return H2_PAL_ERR_IO;
  }
  if (fixture == NULL || fixture->schema != 1u ||
      fixture->size != sizeof(fake_state_t)) {
    return H2_PAL_ERR_FORMAT;
  }
  fake_state_t initial;
  memcpy(&initial, fixture->data, sizeof(initial));
  app->value = initial.value;
  h2_runtime_config_t config = fake_runtime_config();
  const h2_pal_result_t rc = h2_runtime_init(&config, &app->runtime);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  return h2_runtime_input_start(app->runtime, NULL);
}

static h2_runtime_t *fake_runtime(void *user) {
  return ((fake_app_t *)user)->runtime;
}

static h2_pal_result_t fake_run_step(void *user, uint32_t timeout_ms) {
  fake_app_t *app = user;
  uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
  h2_runtime_event_t event = {
      .payload = payload,
      .payload_capacity = sizeof(payload),
  };
  h2_pal_result_t rc =
      h2_runtime_wait_event(app->runtime, &event, timeout_ms);
  if (rc == H2_PAL_OK) {
    app->last_event_kind = event.kind;
    if (event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION &&
        event.payload_size == sizeof(h2_runtime_button_action_event_t)) {
      h2_runtime_button_action_event_t action;
      memcpy(&action, event.payload, sizeof(action));
      app->last_click_count = action.click_count;
    }
    app->value +=
        event.kind == H2_RUNTIME_SYSTEM_EVENT_MODEM_READY ? 5 : 1;
  } else if (rc != H2_PAL_ERR_WOULD_BLOCK && rc != H2_PAL_ERR_TIMEOUT) {
    return rc;
  }
  h2_runtime_button_state_t button_state;
  if (h2_runtime_component_state_button(
          app->runtime, 7u, &button_state) == H2_PAL_OK) {
    app->button_pressed = button_state.pressed;
    app->button_updated_at_ms = button_state.updated_at_ms;
  }
  return app->step_result;
}

static h2_pal_result_t fake_snapshot(
    void *user, h2_app_test_snapshot_writer_t *writer) {
  fake_app_t *app = user;
  if (app->fail_snapshot) {
    return H2_PAL_ERR_IO;
  }
  h2_pal_result_t rc =
      h2_app_test_snapshot_write_i32(writer, "app.value", app->value);
  if (rc == H2_PAL_OK) {
    rc = h2_app_test_snapshot_write_i32(writer, "ui.value", app->value);
  }
  if (rc == H2_PAL_OK) {
    rc = h2_app_test_snapshot_write_i64(
        writer, "app.signed", INT64_MIN);
  }
  if (rc == H2_PAL_OK) {
    rc = h2_app_test_snapshot_write_u64(
        writer, "app.unsigned", UINT64_MAX);
  }
  if (rc == H2_PAL_OK) {
    rc = h2_app_test_snapshot_write_i32(
        writer, "app.last_event_kind", (int32_t)app->last_event_kind);
  }
  if (rc == H2_PAL_OK) {
    rc = h2_app_test_snapshot_write_i32(
        writer, "ui.last_event_kind", (int32_t)app->last_event_kind);
  }
  if (rc == H2_PAL_OK) {
    rc = h2_app_test_snapshot_write_i32(
        writer, "ui.last_click_count", (int32_t)app->last_click_count);
  }
  if (rc == H2_PAL_OK) {
    rc = h2_app_test_snapshot_write_i32(
        writer, "app.button_pressed", app->button_pressed ? 1 : 0);
  }
  if (rc == H2_PAL_OK) {
    rc = h2_app_test_snapshot_write_i32(
        writer, "ui.button_pressed", app->button_pressed ? 1 : 0);
  }
  if (rc == H2_PAL_OK) {
    rc = h2_app_test_snapshot_write_u64(
        writer, "app.button_updated_at_ms", app->button_updated_at_ms);
  }
  if (rc == H2_PAL_OK) {
    rc = h2_app_test_snapshot_write_u64(
        writer, "ui.button_updated_at_ms", app->button_updated_at_ms);
  }
  if (rc != H2_PAL_OK) {
    return rc;
  }
  if (app->corrupt_snapshot == 1u) {
    writer->snapshot->probes[1].kind = (h2_app_test_value_kind_t)0;
  } else if (app->corrupt_snapshot == 2u) {
    writer->snapshot->probes[1].name_offset =
        writer->snapshot->probes[0].name_offset;
    writer->snapshot->probes[1].name_length =
        writer->snapshot->probes[0].name_length;
  }
  return H2_PAL_OK;
}

static void fake_stop(void *user) {
  fake_app_t *app = user;
  if (app->runtime != NULL) {
    h2_runtime_deinit(app->runtime);
    app->runtime = NULL;
  }
  ++app->stop_count;
}

static const h2_app_test_app_vtable_t s_fake_vtable = {
    .reset = fake_reset,
    .runtime = fake_runtime,
    .run_step = fake_run_step,
    .snapshot = fake_snapshot,
    .stop = fake_stop,
};

static h2_app_test_event_t modem_ready_event(uint64_t timestamp_ms) {
  h2_app_test_event_t event;
  assert(h2_app_test_event_init(
             &event,
             H2_RUNTIME_SYSTEM_EVENT_MODEM_READY,
             H2_RUNTIME_COMPONENT_SYSTEM_MODEM,
             H2_RUNTIME_COMPONENT_ID_NONE,
             timestamp_ms,
             NULL,
             0u) == H2_PAL_OK);
  return event;
}

H2_APP_TEST_CASE(fake_sequence_case) {
  const fake_state_t initial = {.value = 2};
  H2_APP_TEST_OPEN(test, "fake", 1u, &initial);
  h2_app_test_event_t event = modem_ready_event(10u);
  H2_APP_TEST_STEP_EVENT(test, event, 10u);
  H2_APP_TEST_EXPECT_I32(test, "app.value", 7);
  H2_APP_TEST_EXPECT_I32(test, "ui.value", 7);
  H2_APP_TEST_RUN(test);
  H2_APP_TEST_EXPECT_I32(test, "app.value", 7);
  H2_APP_TEST_EXPECT_I64(test, "app.signed", INT64_MIN);
  H2_APP_TEST_EXPECT_U64(test, "app.unsigned", UINT64_MAX);
}

static void fake_driver(h2_app_test_driver_t *driver, fake_app_t *app) {
  assert(h2_app_test_memory_driver_init(
             driver,
             (h2_app_test_app_t){
                 .app_id = "fake",
                 .user = app,
                 .vtable = &s_fake_vtable,
             }) == H2_PAL_OK);
}

static h2_app_test_fixture_t fake_fixture(int32_t value) {
  const fake_state_t state = {.value = value};
  h2_app_test_fixture_t fixture;
  assert(h2_app_test_fixture_init(
             &fixture, 1u, &state, sizeof(state)) == H2_PAL_OK);
  return fixture;
}

static void test_fixture_and_event_copy(void) {
  uint8_t payload[] = {1u, 2u, 3u};
  h2_app_test_event_t event;
  assert(h2_app_test_event_init(
             &event,
             H2_RUNTIME_COMPONENT_EVENT_ERROR,
             H2_RUNTIME_COMPONENT_BUTTON,
             7u,
             10u,
             payload,
             sizeof(payload)) == H2_PAL_OK);
  payload[0] = 9u;
  assert(event.payload[0] == 1u);
  assert(h2_app_test_event_init(
             &event,
             H2_RUNTIME_COMPONENT_EVENT_ERROR,
             H2_RUNTIME_COMPONENT_BUTTON,
             7u,
             10u,
             payload,
             H2_RUNTIME_EVENT_PAYLOAD_MAX + 1u) ==
         H2_PAL_ERR_INVALID_ARG);

  h2_app_test_fixture_t fixture;
  assert(h2_app_test_fixture_init(
             &fixture, 0u, payload, sizeof(payload)) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_app_test_fixture_init(
             &fixture, 1u, payload, H2_APP_TEST_FIXTURE_MAX + 1u) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_app_test_fixture_init(&fixture, 1u, NULL, 1u) ==
         H2_PAL_ERR_INVALID_ARG);
}

static void test_snapshot_boundaries(void) {
  h2_app_test_snapshot_t snapshot = {0};
  h2_app_test_snapshot_writer_t writer = {.snapshot = &snapshot};
  assert(h2_app_test_snapshot_write_i32(&writer, "", 1) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_app_test_snapshot_write_i32(&writer, "value", 3) ==
         H2_PAL_OK);
  assert(h2_app_test_snapshot_write_i32(&writer, "value", 4) ==
         H2_PAL_ERR_FORMAT);
  int32_t value = 0;
  assert(h2_app_test_snapshot_get_i32(&snapshot, "value", &value) ==
         H2_PAL_OK);
  assert(value == 3);
  assert(h2_app_test_snapshot_get_i32(&snapshot, "missing", &value) ==
         H2_PAL_ERR_NOT_FOUND);
  assert(h2_app_test_snapshot_write_bool(&writer, "bool", true) ==
         H2_PAL_OK);
  assert(h2_app_test_snapshot_write_u32(&writer, "u32", UINT32_MAX) ==
         H2_PAL_OK);
  assert(h2_app_test_snapshot_write_i64(&writer, "i64", INT64_MIN) ==
         H2_PAL_OK);
  assert(h2_app_test_snapshot_write_u64(&writer, "u64", UINT64_MAX) ==
         H2_PAL_OK);
  assert(h2_app_test_snapshot_write_string(&writer, "string", "value") ==
         H2_PAL_OK);
}

static void test_memory_sequence_and_cleanup(void) {
  fake_app_t app = {0};
  h2_app_test_driver_t driver;
  fake_driver(&driver, &app);
  h2_app_test_fixture_t fixture = fake_fixture(2);
  h2_app_test_session_t *session = NULL;
  assert(h2_app_test_session_open(&driver, "missing", &fixture, &session) ==
         H2_PAL_ERR_NOT_FOUND);
  assert(session == NULL);
  assert(h2_app_test_session_open(&driver, "fake", &fixture, &session) ==
         H2_PAL_OK);
  assert(app.reset_count == 1u);

  h2_app_test_event_t event = modem_ready_event(10u);
  h2_app_test_snapshot_t snapshot;
  assert(h2_app_test_session_emit_event(
             session, &event, 0u, &snapshot) == H2_PAL_OK);
  assert(snapshot.generation == 1u);
  assert(snapshot.step_result == H2_PAL_OK);
  int32_t value = 0;
  assert(h2_app_test_snapshot_get_i32(
             &snapshot, "app.value", &value) == H2_PAL_OK);
  assert(value == 7);
  assert(h2_app_test_session_run(session, 0u, &snapshot) == H2_PAL_OK);
  assert(snapshot.generation == 2u);

  h2_app_test_session_close(session);
  h2_app_test_session_close(session);
  assert(app.stop_count == 1u);
  assert(h2_app_test_session_open(&driver, "fake", &fixture, &session) ==
         H2_PAL_OK);
  h2_app_test_memory_driver_deinit(&driver);
  assert(app.stop_count == 2u);
}

static void test_failure_cleanup_and_snapshot_validation(void) {
  fake_app_t app = {.fail_reset = true};
  h2_app_test_driver_t driver;
  fake_driver(&driver, &app);
  h2_app_test_fixture_t fixture = fake_fixture(2);
  h2_app_test_session_t *session = NULL;
  assert(h2_app_test_session_open(&driver, "fake", &fixture, &session) ==
         H2_PAL_ERR_IO);
  assert(app.stop_count == 1u);

  app.fail_reset = false;
  assert(h2_app_test_session_open(&driver, "fake", &fixture, &session) ==
         H2_PAL_OK);
  app.fail_snapshot = true;
  h2_app_test_snapshot_t snapshot;
  assert(h2_app_test_session_run(session, 0u, &snapshot) == H2_PAL_ERR_IO);
  app.fail_snapshot = false;
  app.step_result = H2_PAL_ERR_UNSUPPORTED;
  assert(h2_app_test_session_run(session, 0u, &snapshot) == H2_PAL_OK);
  assert(snapshot.step_result == H2_PAL_ERR_UNSUPPORTED);
  h2_app_test_session_close(session);

  for (unsigned int corruption = 1u; corruption <= 2u; ++corruption) {
    app.step_result = H2_PAL_OK;
    app.corrupt_snapshot = corruption;
    assert(h2_app_test_session_open(&driver, "fake", &fixture, &session) ==
           H2_PAL_OK);
    assert(h2_app_test_session_run(session, 0u, &snapshot) ==
           H2_PAL_ERR_FORMAT);
    h2_app_test_session_close(session);
  }
  h2_app_test_memory_driver_deinit(&driver);
}

static void test_memory_semantic_button_and_component_state_operations(void) {
  fake_app_t app = {0};
  h2_app_test_driver_t driver;
  fake_driver(&driver, &app);
  h2_app_test_fixture_t fixture = fake_fixture(0);
  h2_app_test_session_t *session = NULL;
  h2_app_test_snapshot_t snapshot;
  assert(h2_app_test_session_open(
             &driver, "fake", &fixture, &session) == H2_PAL_OK);

  assert(h2_app_test_session_button_down(
             session, 7u, 100u, 0u, &snapshot) == H2_PAL_OK);
  assert(snapshot.generation == 1u);
  int32_t value = 0;
  assert(h2_app_test_snapshot_get_i32(
             &snapshot, "app.last_event_kind", &value) == H2_PAL_OK);
  assert(value == H2_RUNTIME_COMPONENT_EVENT_BUTTON_DOWN);
  assert(h2_app_test_snapshot_get_i32(
             &snapshot, "ui.button_pressed", &value) == H2_PAL_OK);
  assert(value == 1);

  h2_runtime_button_state_t state = {
      .pressed = false,
      .updated_at_ms = 150u,
      .result = H2_PAL_OK,
  };
  assert(h2_app_test_session_set_component_state(
             session, 7u, &state, sizeof(state), 0u,
             &snapshot) == H2_PAL_OK);
  assert(snapshot.generation == 2u);
  assert(h2_app_test_snapshot_get_i32(
             &snapshot, "app.button_pressed", &value) == H2_PAL_OK);
  assert(value == 0);
  uint64_t updated_at_ms = 0u;
  assert(h2_app_test_snapshot_get_u64(
             &snapshot, "ui.button_updated_at_ms",
             &updated_at_ms) == H2_PAL_OK);
  assert(updated_at_ms == 150u);

  assert(h2_app_test_session_button_action(
             session, 7u, 200u, 240u, 1u, 0u,
             &snapshot) == H2_PAL_OK);
  assert(snapshot.generation == 3u);
  assert(h2_app_test_snapshot_get_i32(
             &snapshot, "ui.last_event_kind", &value) == H2_PAL_OK);
  assert(value == H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION);
  assert(h2_app_test_snapshot_get_i32(
             &snapshot, "ui.last_click_count", &value) == H2_PAL_OK);
  assert(value == 1);

  /* Consecutive-click counts reach the App exactly as Runtime would send them. */
  assert(h2_app_test_session_button_action(
             session, 7u, 260u, 280u, 3u, 0u,
             &snapshot) == H2_PAL_OK);
  assert(snapshot.generation == 4u);
  assert(h2_app_test_snapshot_get_i32(
             &snapshot, "ui.last_click_count", &value) == H2_PAL_OK);
  assert(value == 3);
  assert(h2_app_test_session_button_action(
             session, 7u, 300u, 320u, 0u, 0u,
             &snapshot) == H2_PAL_ERR_INVALID_ARG);

  assert(h2_app_test_session_button_up(
             session, 7u, 300u, 299u, 0u,
             &snapshot) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_app_test_session_run(
             session, 0u, &snapshot) == H2_PAL_OK);
  assert(snapshot.generation == 5u);

  h2_app_test_session_close(session);
  h2_app_test_memory_driver_deinit(&driver);
}

static void test_case_macros(void) {
  fake_app_t app = {0};
  h2_app_test_driver_t driver;
  fake_driver(&driver, &app);
  assert(fake_sequence_case(&driver) == H2_PAL_OK);
  assert(app.stop_count == 1u);
  h2_app_test_memory_driver_deinit(&driver);
}

int main(void) {
  test_fixture_and_event_copy();
  test_snapshot_boundaries();
  test_memory_sequence_and_cleanup();
  test_failure_cleanup_and_snapshot_validation();
  test_memory_semantic_button_and_component_state_operations();
  test_case_macros();
  return 0;
}
