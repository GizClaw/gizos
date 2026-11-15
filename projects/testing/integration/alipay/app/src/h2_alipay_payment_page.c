#include "h2_alipay_payment_page.h"
#include "h2_alipay_payment_model.h"
#include "h2_atomic.h"
#include "h2_lvgl_touch.h"
#include "h2_qrcode.h"
#include <stdio.h>
#include <string.h>

typedef enum mode { MENU, CODE, TAP, CONFIRM, RESULT } payment_mode_t;
typedef enum action {
  A_CODE = 1,
  A_TAP,
  A_REFRESH,
  A_CONFIRM,
  A_BACK
} action_t;
typedef enum job {
  J_NONE,
  J_CODE,
  J_QUERY,
  J_TAP_START,
  J_FRAME,
  J_CONFIRM,
  J_TAP_QUERY,
  J_BLE_RESULT,
  J_STOP
} job_t;
typedef struct payment_page {
  h2_runtime_t* rt;
  const h2_alipay_e2e_config_t* config;
  const h2_alipay_payment_api_t* api;
  lv_display_t* display;
  h2_lvgl_touch_t* touch;
  lv_group_t* button_group;
  bool buttons_enabled;
  lv_obj_t *title, *status, *qr, *buttons[5];
  uint16_t* qr_pixels;
  int width, height;
  payment_mode_t mode;
  unsigned actions[8], head, count;
  bool queue_full, leaving, exiting, tap_active, scanning, stay_on_result;
  char terminal_message[96];
  int (*presented)(void*);
  void* presented_user;
  job_t job;
  h2_pal_task_t* worker;
  h2_atomic_bool_t done;
  h2_atomic_flag_t ble_lock;
  bool ble_pending;
  uint8_t ble_mailbox[27], job_ble[27];
  uint8_t uid[H2_PAL_NFC_UID_MAX_LEN], uid_length;
  uint8_t generated[H2_ALIPAY_PAYMENT_CODE_CAPACITY];
  uint32_t generated_length, generation, job_generation;
  int call_rc, sdk_rc;
  h2_alipay_payment_result_t result;
  h2_alipay_payment_model_t model;
  h2_alipay_payment_attempt_t query;
  h2_pal_ble_adv_set_t* advertising;
  uint8_t manufacturer[33], service_uuid[16];
  h2_pal_ble_uuid_t advertised_uuid;
  uint64_t tap_deadline, query_due, last_tick;
  uint64_t code_wall_ms, code_mono_ms, diagnostic_ms;
  unsigned key_events;
} payment_page_t;

static void wipe(void* data, size_t size) {
  volatile uint8_t* p = data;
  while (size-- != 0u) *p++ = 0u;
}
static uint64_t now_ms(payment_page_t* p) {
  uint64_t now = 0u;
  (void)h2_pal_time_get_monotonic_ms(p->rt->time, &now);
  return now;
}
static void enqueue(payment_page_t* p, action_t action) {
  if (p->count == 8u) {
    p->queue_full = true;
    return;
  }
  p->actions[(p->head + p->count++) % 8u] = action;
}
static void code_clicked(lv_event_t* e) {
  enqueue(lv_event_get_user_data(e), A_CODE);
}
static void tap_clicked(lv_event_t* e) {
  enqueue(lv_event_get_user_data(e), A_TAP);
}
static void refresh_clicked(lv_event_t* e) {
  enqueue(lv_event_get_user_data(e), A_REFRESH);
}
static void confirm_clicked(lv_event_t* e) {
  enqueue(lv_event_get_user_data(e), A_CONFIRM);
}
static void back_clicked(lv_event_t* e) {
  enqueue(lv_event_get_user_data(e), A_BACK);
}
static void status(payment_page_t* p, const char* text) {
  lv_label_set_text(p->status, text);
}
static void clear_qr(payment_page_t* p) {
  if (p->qr != NULL) lv_obj_delete(p->qr);
  p->qr = NULL;
  if (p->qr_pixels != NULL) {
    /* Canvas geometry is bounded by the display; clear the whole allocation. */
    size_t bytes =
        lv_draw_buf_width_to_stride(p->width, LV_COLOR_FORMAT_RGB565) *
        (size_t)p->height;
    wipe(p->qr_pixels, bytes);
    h2_pal_mem_free(p->rt->mem, p->qr_pixels);
  }
  p->qr_pixels = NULL;
}
static void controls(payment_page_t* p) {
  lv_obj_set_pos(p->status, 8,
                 p->mode == MENU ? p->height - 60 : p->height - 88);
  for (unsigned i = 0u; i < 5u; ++i) lv_obj_set_hidden(p->buttons[i], true);
  if (p->mode == MENU) {
    lv_obj_set_hidden(p->buttons[0], false);
    lv_obj_set_hidden(p->buttons[1], p->api->vtable->start_tap == NULL);
  } else {
    lv_obj_set_hidden(p->buttons[4], false);
    if (p->mode == CODE) lv_obj_set_hidden(p->buttons[2], false);
    if (p->mode == CONFIRM) lv_obj_set_hidden(p->buttons[3], false);
  }
  for (unsigned i = 0u; i < 4u; ++i) {
    if (p->worker != NULL || p->leaving)
      lv_obj_add_state(p->buttons[i], LV_STATE_DISABLED);
    else
      lv_obj_remove_state(p->buttons[i], LV_STATE_DISABLED);
  }
  if (p->button_group != NULL) {
    lv_obj_t* focused = lv_group_get_focused(p->button_group);
    if (focused == NULL || lv_obj_is_hidden(focused) ||
        lv_obj_has_state(focused, LV_STATE_DISABLED)) {
      for (unsigned i = 0u; i < 5u; ++i)
        if (!lv_obj_is_hidden(p->buttons[i]) &&
            !lv_obj_has_state(p->buttons[i], LV_STATE_DISABLED)) {
          lv_group_focus_obj(p->buttons[i]);
          break;
        }
    }
  }
}
static int open_input(payment_page_t* p) {
  const h2_alipay_payment_buttons_t* keys = &p->config->payment_buttons;
  const h2_runtime_component_id_t ids[] = {keys->previous, keys->next,
                                           keys->activate, keys->back};
  bool any = false;
  for (size_t i = 0u; i < 4u; ++i) any = any || ids[i] != 0u;
  if (any) {
    for (size_t i = 0u; i < 4u; ++i) {
      h2_runtime_component_info_t info;
      int rc = h2_runtime_component_get(p->rt, ids[i], &info);
      if (rc != 0) return rc;
      if (info.kind != H2_RUNTIME_COMPONENT_BUTTON)
        return H2_PAL_ERR_INVALID_ARG;
      for (size_t j = 0u; j < i; ++j)
        if (ids[i] == ids[j]) return H2_PAL_ERR_INVALID_ARG;
    }
    p->buttons_enabled = true;
    p->button_group = lv_group_create();
    if (p->button_group == NULL) return H2_PAL_ERR_NO_MEMORY;
    for (unsigned i = 0u; i < 5u; ++i)
      lv_group_add_obj(p->button_group, p->buttons[i]);
  }
  const h2_lvgl_touch_config_t touch = {
      .touch = p->rt->touch, .allocator = p->rt->mem, .display = p->display};
  int rc = h2_lvgl_touch_create(&touch, &p->touch);
  printf("H2_ALIPAY_PAYMENT_INPUT touch_rc=%d buttons=%u\n", rc,
         p->buttons_enabled ? 1u : 0u);
  return rc == H2_PAL_ERR_UNSUPPORTED && p->buttons_enabled ? H2_PAL_OK : rc;
}
static int focused_index(payment_page_t* p) {
  if (p->button_group == NULL) return -1;
  lv_obj_t* focused = lv_group_get_focused(p->button_group);
  for (unsigned i = 0u; i < 5u; ++i)
    if (focused == p->buttons[i]) return (int)i;
  return -1;
}
static void input_status(payment_page_t* p, uint64_t now) {
  if (now < p->diagnostic_ms) return;
  p->diagnostic_ms = now + 10000u;
  h2_runtime_input_status_t input;
  int rc = h2_runtime_input_status(p->rt, &input);
  char line[256];
  if (rc == H2_PAL_OK)
    (void)snprintf(
        line, sizeof(line),
        "H2_ALIPAY_PAYMENT_INPUT_STATUS phase=%u worker_result=%d last_error=%d "
        "poll_count=%u error_count=%u key_events=%u focused=%d mode=%u",
        (unsigned)input.phase, input.worker_result, input.last_error,
        (unsigned)input.poll_count, (unsigned)input.error_count, p->key_events,
        focused_index(p), (unsigned)p->mode);
  else
    (void)snprintf(line, sizeof(line), "H2_ALIPAY_PAYMENT_INPUT_STATUS rc=%d",
                   rc);
  (void)h2_pal_log_write(p->rt->log, H2_PAL_LOG_INFO, "alipay/payment", line);
}
static void button_event(payment_page_t* p, const h2_runtime_event_t* event) {
  if (!p->buttons_enabled || p->leaving ||
      event->kind != H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION ||
      event->component != H2_RUNTIME_COMPONENT_BUTTON ||
      event->payload_size != sizeof(h2_runtime_button_action_event_t))
    return;
  h2_runtime_button_action_event_t action;
  memcpy(&action, event->payload, sizeof(action));
  if (!h2_runtime_button_action_is_released(&action)) return;
  const h2_alipay_payment_buttons_t* keys = &p->config->payment_buttons;
  unsigned key_action;
  if (event->component_id == keys->back) {
    key_action = 4u;
    if (p->mode != MENU) enqueue(p, A_BACK);
  } else if (event->component_id == keys->previous) {
    key_action = 1u;
    lv_group_focus_prev(p->button_group);
  } else if (event->component_id == keys->next) {
    key_action = 2u;
    lv_group_focus_next(p->button_group);
  } else if (event->component_id == keys->activate) {
    key_action = 3u;
    if (p->worker == NULL) {
      lv_obj_t* focused = lv_group_get_focused(p->button_group);
      if (focused != NULL && !lv_obj_is_hidden(focused) &&
          !lv_obj_has_state(focused, LV_STATE_DISABLED))
        (void)lv_obj_send_event(focused, LV_EVENT_CLICKED, NULL);
    }
  } else
    return;
  ++p->key_events;
  char line[144];
  (void)snprintf(
      line, sizeof(line),
      "H2_ALIPAY_PAYMENT_KEY component=%u action=%u focused=%d busy=%u",
      (unsigned)event->component_id, key_action, focused_index(p),
      p->worker != NULL ? 1u : 0u);
  (void)h2_pal_log_write(p->rt->log, H2_PAL_LOG_INFO, "alipay/payment", line);
}
static int show_qr(payment_page_t* p) {
  clear_qr(p);
  uint8_t modules[H2_QRCODE_BUFFER_LEN_FOR_VERSION(10)],
      scratch[sizeof(modules)];
  h2_qrcode_t code = {0};
  h2_qrcode_layout_t layout;
  int rc = h2_qrcode_encode_text(p->model.visible, H2_QRCODE_ECC_QUARTILE, 10,
                                 modules, sizeof(modules), scratch,
                                 sizeof(scratch), &code);
  if (rc != 0) return rc;
  rc = h2_qrcode_layout_center(&code, p->width - 16, p->height - 100, 4,
                               &layout);
  if (rc != 0) return rc;
  int span = (code.size + 8) * layout.scale;
  size_t stride = lv_draw_buf_width_to_stride(p->width, LV_COLOR_FORMAT_RGB565);
  p->qr_pixels = h2_pal_mem_alloc(p->rt->mem, stride * (size_t)p->height);
  if (p->qr_pixels == NULL) return H2_PAL_ERR_NO_MEMORY;
  memset(p->qr_pixels, 0xff, stride * (size_t)p->height);
  int canvas_width =
      (int)(lv_draw_buf_width_to_stride(span, LV_COLOR_FORMAT_RGB565) / 2u);
  layout.origin_x = layout.origin_y = 0;
  rc = h2_qrcode_render_rgb565_band(&code, &layout, 0, UINT16_MAX, UINT16_MAX,
                                    0, canvas_width, span, p->qr_pixels,
                                    stride * (size_t)p->height / 2u);
  if (rc != 0) {
    clear_qr(p);
    return rc;
  }
  p->qr = lv_canvas_create(lv_screen_active());
  if (p->qr == NULL) {
    clear_qr(p);
    return H2_PAL_ERR_NO_MEMORY;
  }
  lv_canvas_set_buffer(p->qr, p->qr_pixels, span, span, LV_COLOR_FORMAT_RGB565);
  lv_obj_set_pos(p->qr, (p->width - span) / 2, 32);
  return 0;
}
static int stop_radio(payment_page_t* p) {
  int rc = 0;
  if (p->scanning) {
    rc = h2_pal_ble_stop_scan(p->rt->ble_host);
    if (rc != 0) return rc;
    p->scanning = false;
  }
  if (p->advertising != NULL) {
    int stopped = h2_pal_ble_adv_set_stop(p->rt->ble_host, p->advertising);
    if (stopped != 0) return stopped;
    rc = h2_pal_ble_adv_set_destroy(p->rt->ble_host, p->advertising);
    if (rc == 0) {
      p->advertising = NULL;
      wipe(p->manufacturer, sizeof(p->manufacturer));
      wipe(p->service_uuid, sizeof(p->service_uuid));
      p->advertised_uuid = (h2_pal_ble_uuid_t){0};
    }
  }
  return rc;
}
static bool scan_result(void* user, const h2_pal_ble_scan_result_t* result) {
  payment_page_t* p = user;
  uint8_t payload[27];
  if (result->data_status != H2_PAL_BLE_ADV_DATA_COMPLETE ||
      h2_alipay_payment_ble_result(result->raw_data.data, result->raw_data.len,
                                   payload) != 0)
    return false;
  if (h2_atomic_flag_test_and_set(&p->ble_lock, H2_ATOMIC_ACQUIRE))
    return false;
  if (!p->ble_pending) {
    memcpy(p->ble_mailbox, payload, sizeof(payload));
    p->ble_pending = true;
  }
  h2_atomic_flag_clear(&p->ble_lock, H2_ATOMIC_RELEASE);
  (void)h2_runtime_notify(p->rt);
  return false;
}
static int broadcast(void* user, uint16_t manufacturer, const uint8_t* data,
                     size_t length, const uint8_t* service,
                     size_t service_length, bool quick) {
  payment_page_t* p = user;
  (void)quick;
  if (length > 31u || (service_length != 2u && service_length != 16u))
    return H2_PAL_ERR_FORMAT;
  if (p->advertising != NULL) return H2_PAL_ERR_BUSY;
  p->manufacturer[0] = (uint8_t)manufacturer;
  p->manufacturer[1] = (uint8_t)(manufacturer >> 8u);
  memcpy(p->manufacturer + 2u, data, length);
  memcpy(p->service_uuid, service, service_length);
  p->advertised_uuid = (h2_pal_ble_uuid_t){p->service_uuid, service_length};
  h2_pal_ble_adv_params_t params = {.mode = H2_PAL_BLE_ADV_MODE_NON_CONNECTABLE,
                                    .interval_min_ms = 32u,
                                    .interval_max_ms = 48u,
                                    .type = length + service_length + 9u <= 31u
                                                ? H2_PAL_BLE_ADV_TYPE_LEGACY
                                                : H2_PAL_BLE_ADV_TYPE_EXTENDED,
                                    .primary_phy = H2_PAL_BLE_PHY_1M,
                                    .secondary_phy = H2_PAL_BLE_PHY_1M,
                                    .sid = 1u};
  int rc = h2_pal_ble_adv_set_create(p->rt->ble_host, &params, &p->advertising);
  if (rc != 0) return rc;
  const h2_pal_ble_adv_data_t adv = {
      .manufacturer_data = {p->manufacturer, length + 2u},
      .service_uuids = &p->advertised_uuid,
      .service_uuid_count = 1u};
  rc = h2_pal_ble_adv_set_set_data(p->rt->ble_host, p->advertising, &adv);
  if (rc == 0) rc = h2_pal_ble_adv_set_start(p->rt->ble_host, p->advertising);
  if (rc != 0) {
    int removed = h2_pal_ble_adv_set_destroy(p->rt->ble_host, p->advertising);
    if (removed == 0) p->advertising = NULL;
  }
  return rc;
}
static void work(void* user) {
  payment_page_t* p = user;
  const h2_alipay_payment_vtable_t* api = p->api->vtable;
  p->result =
      (h2_alipay_payment_result_t){.sdk_rc = INT_MIN, .sdk_status = INT_MIN};
  p->sdk_rc = INT_MIN;
  switch (p->job) {
    case J_CODE:
      p->generated_length = sizeof(p->generated);
      p->call_rc = api->get_code(p->api->user, p->generated,
                                 &p->generated_length, &p->sdk_rc);
      break;
    case J_QUERY:
      p->call_rc = api->query_code(p->api->user, p->query.code, &p->result);
      break;
    case J_TAP_START: {
      if (api->start_tap == NULL) {
        p->call_rc = H2_PAL_ERR_UNSUPPORTED;
        break;
      }
      p->call_rc = h2_pal_ble_start(p->rt->ble_host);
      if (p->call_rc != 0) break;
      const h2_pal_ble_scan_params_t scan = {
          .mode = H2_PAL_BLE_SCAN_MODE_PASSIVE,
          .interval_ms = 80u,
          .window_ms = 60u};
      p->call_rc =
          h2_pal_ble_start_scan(p->rt->ble_host, &scan, scan_result, p);
      if (p->call_rc != 0) break;
      p->scanning = true;
      const h2_alipay_tap_transport_t transport = {
          .user = p, .broadcast_direct = broadcast};
      p->call_rc = api->start_tap(p->api->user, &transport, &p->result);
      p->tap_active = p->call_rc == 0;
      break;
    }
    case J_FRAME: {
      h2_pal_periph_id_t id;
      h2_pal_nfc_data_read_t data = {0};
      p->call_rc =
          h2_runtime_periph_id(p->rt, p->config->payment_nfc_component_id, &id);
      if (p->call_rc == 0)
        p->call_rc =
            h2_pal_nfc_read_nfc_data(p->rt->nfc, id, p->uid, p->uid_length,
                                     H2_PAL_NFC_DATA_NDEF, p->rt->mem, &data);
      if (p->call_rc == 0)
        p->call_rc =
            api->receive_ndef(p->api->user, data.bytes, data.len, &p->result);
      if (data.bytes != NULL) {
        wipe(data.bytes, data.len);
        h2_pal_mem_free(p->rt->mem, data.bytes);
      }
      break;
    }
    case J_CONFIRM:
      p->call_rc = api->confirm_tap(p->api->user, &p->result);
      break;
    case J_TAP_QUERY:
      p->call_rc = api->query_tap(p->api->user, &p->result);
      break;
    case J_BLE_RESULT:
      p->call_rc = api->receive_ble_result(p->api->user, p->job_ble,
                                           sizeof(p->job_ble), &p->result);
      break;
    case J_STOP:
      p->call_rc = p->tap_active ? api->stop_tap(p->api->user) : 0;
      if (p->call_rc == 0) {
        p->tap_active = false;
        p->call_rc = stop_radio(p);
      }
      break;
    default:
      p->call_rc = H2_PAL_ERR_INVALID_STATE;
      break;
  }
  h2_atomic_bool_store(&p->done, true, H2_ATOMIC_RELEASE);
  (void)h2_runtime_notify(p->rt);
}
static int start_job(payment_page_t* p, job_t job) {
  if (p->worker != NULL) return H2_PAL_ERR_BUSY;
  p->job = job;
  p->job_generation = p->generation;
  h2_atomic_bool_store(&p->done, false, H2_ATOMIC_RELEASE);
  const h2_pal_task_options_t options = {.name = "alipay/e2e/sdk",
                                         .min_stack_size = 65536u};
  int rc = h2_pal_task_start(p->rt->task, &options, work, p, &p->worker);
  if (rc != 0) {
    p->job = J_NONE;
    status(p, "Cannot start operation");
  }
  controls(p);
  return rc;
}
static void paid(payment_page_t* p) {
  clear_qr(p);
  p->mode = RESULT;
  char text[96];
  (void)snprintf(text, sizeof(text), "Payment confirmed\n%s", p->result.amount);
  status(p, text);
  (void)snprintf(p->terminal_message, sizeof(p->terminal_message), "%s", text);
  p->stay_on_result = p->tap_active || p->scanning || p->advertising != NULL;
  h2_alipay_payment_model_clear(&p->model);
}
static void finish_job(payment_page_t* p) {
  job_t job = p->job;
  p->job = J_NONE;
  printf(
      "H2_ALIPAY_PAYMENT operation=%u pal_rc=%d sdk_rc=%d sdk_status=%d state=%d nfc_rc=%d ble_rc=%d sent=%d confirm=%d code_length=%u\n",
      (unsigned)job, p->call_rc, job == J_CODE ? p->sdk_rc : p->result.sdk_rc,
      p->result.sdk_status, p->result.state, p->result.nfc_write_rc,
      p->result.ble_send_rc, p->result.response_sent,
      p->result.confirmation_required,
      job == J_CODE ? (unsigned)p->generated_length : 0u);
  if (job == J_STOP) {
    if (p->call_rc == 0) {
      bool retain = p->stay_on_result && !p->leaving && !p->exiting;
      p->stay_on_result = false;
      if (retain) {
        p->mode = RESULT;
        status(p, p->terminal_message);
      } else {
        p->leaving = false;
        p->mode = MENU;
        clear_qr(p);
        h2_alipay_payment_model_clear(&p->model);
        status(p, "Choose payment method");
      }
    } else
      status(p, "Stopping radio; waiting for cleanup");
  } else if (p->job_generation == p->generation && !p->leaving) {
    if (job == J_CODE && p->mode == CODE) {
      h2_alipay_payment_model_t previous = p->model;
      int rc = p->call_rc;
      if (rc == 0 && p->sdk_rc == 0)
        rc = h2_alipay_payment_model_code(&p->model, p->generated,
                                          p->generated_length, now_ms(p));
      else if (rc == 0)
        rc = H2_PAL_ERR_IO;
      if (rc == 0) rc = show_qr(p);
      if (rc == 0) {
        lv_refr_now(p->display);
        if (p->presented != NULL) rc = p->presented(p->presented_user);
      }
      if (rc == 0) {
        p->code_mono_ms = now_ms(p);
        p->code_wall_ms = 0u;
        (void)h2_pal_time_get_wall_ms(p->rt->time, &p->code_wall_ms);
        status(p, "Show code to merchant");
      } else {
        clear_qr(p);
        p->model = previous;
        wipe(p->model.visible, sizeof(p->model.visible));
        p->model.length = 0u;
        char text[80];
        (void)snprintf(text, sizeof(text), "Code unavailable\nSDK: %d / rc: %d",
                       p->sdk_rc, rc);
        status(p, text);
        p->model.refresh_ms = now_ms(p) + 60000u;
      }
      wipe(&previous, sizeof(previous));
    } else if (job == J_TAP_START) {
      if (p->call_rc == 0) {
        status(p, "Tap the payment terminal");
        p->tap_deadline = now_ms(p) + 60000u;
      } else {
        status(p, "Tap unavailable; see status log");
        p->mode = RESULT;
      }
    } else if (job == J_FRAME || job == J_CONFIRM) {
      if (p->result.confirmation_required) {
        p->mode = CONFIRM;
        status(p, "Confirm this tap payment?");
      } else if (p->call_rc == 0 && p->result.response_sent) {
        p->mode = TAP;
        p->query_due = now_ms(p) + 2000u;
        status(p, "Response sent; checking payment");
      } else {
        p->mode = RESULT;
        status(p, "Tap not completed; check merchant");
      }
    } else if (job == J_QUERY || job == J_TAP_QUERY || job == J_BLE_RESULT) {
      bool belongs =
          job != J_QUERY || h2_alipay_payment_model_has(&p->model, p->query.id);
      if (p->call_rc == 0 && belongs &&
          p->result.state == H2_ALIPAY_PAYMENT_PAID)
        paid(p);
      else if (p->call_rc == 0 && belongs &&
               (p->result.state == H2_ALIPAY_PAYMENT_UNBOUND ||
                p->result.state == H2_ALIPAY_PAYMENT_DISABLED ||
                p->result.state == H2_ALIPAY_PAYMENT_REJECTED)) {
        clear_qr(p);
        p->mode = RESULT;
        status(p, "Payment unavailable; check account");
      }
    }
  }
  if (p->mode == RESULT && !p->leaving &&
      (p->tap_active || p->scanning || p->advertising != NULL)) {
    if (!p->stay_on_result)
      (void)snprintf(p->terminal_message, sizeof(p->terminal_message), "%s",
                     lv_label_get_text(p->status));
    p->stay_on_result = true;
  }
  wipe(p->generated, sizeof(p->generated));
  p->generated_length = 0u;
  wipe(p->job_ble, sizeof(p->job_ble));
  wipe(&p->query, sizeof(p->query));
  controls(p);
}
static lv_obj_t* make_button(payment_page_t* p, const char* text, int x, int y,
                             int width, lv_event_cb_t callback) {
  lv_obj_t* button = lv_button_create(lv_screen_active());
  if (button == NULL) return NULL;
  lv_obj_remove_style_all(button);
  lv_obj_set_pos(button, x, y);
  lv_obj_set_size(button, width, 38);
  /* Runtime navigation has no keypad indev to set LV_STATE_FOCUS_KEY.
   * Give its ordinary focus an explicit, visible selection. */
  lv_obj_set_style_radius(button, 6, LV_STATE_DEFAULT);
  lv_obj_set_style_bg_color(button, lv_color_hex(0xe5e7eb), LV_STATE_DEFAULT);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_STATE_DEFAULT);
  lv_obj_set_style_text_color(button, lv_color_hex(0x111111), LV_STATE_DEFAULT);
  lv_obj_set_style_bg_color(button, lv_color_hex(0x1456b8), LV_STATE_FOCUSED);
  lv_obj_set_style_text_color(button, lv_color_hex(0xffffff), LV_STATE_FOCUSED);
  lv_obj_set_style_outline_color(button, lv_color_hex(0x1456b8),
                                 LV_STATE_FOCUSED);
  lv_obj_set_style_outline_width(button, 2, LV_STATE_FOCUSED);
  lv_obj_set_style_outline_pad(button, 2, LV_STATE_FOCUSED);
  lv_obj_set_style_outline_opa(button, LV_OPA_COVER, LV_STATE_FOCUSED);
  lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, p);
  lv_obj_t* label = lv_label_create(button);
  if (label == NULL) {
    lv_obj_delete(button);
    return NULL;
  }
  lv_label_set_text(label, text);
  lv_obj_center(label);
  return button;
}
int h2_alipay_payment_page_run(h2_runtime_t* rt,
                               const h2_alipay_e2e_config_t* config,
                               lv_display_t* display, int width, int height,
                               int (*presented)(void*), void* presented_user) {
  if (rt == NULL || config == NULL || config->payment == NULL ||
      config->payment->vtable == NULL ||
      config->payment->vtable->get_code == NULL ||
      config->payment->vtable->query_code == NULL || display == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  payment_page_t* p = h2_pal_mem_alloc(rt->mem, sizeof(*p));
  if (p == NULL) return H2_PAL_ERR_NO_MEMORY;
  memset(p, 0, sizeof(*p));
  p->rt = rt;
  p->config = config;
  p->api = config->payment;
  p->display = display;
  p->width = width;
  p->height = height;
  p->presented = presented;
  p->presented_user = presented_user;
  bool atomic_open = false, flag_open = false;
  int rc = 0;
  if (h2_atomic_bool_init(&p->done, false) != H2_ATOMIC_OK) {
    rc = H2_PAL_ERR_NO_MEMORY;
    goto done;
  }
  atomic_open = true;
  if (h2_atomic_flag_init(&p->ble_lock) != H2_ATOMIC_OK) {
    rc = H2_PAL_ERR_NO_MEMORY;
    goto done;
  }
  flag_open = true;
  lv_obj_clean(lv_screen_active());
  p->title = lv_label_create(lv_screen_active());
  p->status = lv_label_create(lv_screen_active());
  if (p->title == NULL || p->status == NULL) {
    rc = H2_PAL_ERR_NO_MEMORY;
    goto done;
  }
  lv_label_set_text(p->title,
                    config->alipay == NULL ? "Alipay / controlled"
                    : config->alipay->storage_scope == H2_ALIPAY_STORAGE_SESSION
                        ? "Alipay / session"
                        : "Alipay / persistent");
  lv_obj_set_pos(p->title, 8, 6);
  lv_obj_set_pos(p->status, 8, height - 88);
  lv_obj_set_width(p->status, width - 16);
  p->buttons[0] = make_button(p, "Pay code", 20, 62, width - 40, code_clicked);
  p->buttons[1] =
      make_button(p, "Tap to pay", 20, 118, width - 40, tap_clicked);
  p->buttons[2] = make_button(p, "Refresh", 8, height - 44, (width - 24) / 2,
                              refresh_clicked);
  p->buttons[3] = make_button(p, "Confirm", 8, height - 44, (width - 24) / 2,
                              confirm_clicked);
  p->buttons[4] = make_button(p, "Back", width / 2 + 4, height - 44,
                              (width - 24) / 2, back_clicked);
  for (unsigned i = 0u; i < 5u; ++i)
    if (p->buttons[i] == NULL) {
      rc = H2_PAL_ERR_NO_MEMORY;
      goto done;
    }
  rc = open_input(p);
  if (rc != 0) goto done;
  status(p, "Choose payment method");
  controls(p);
  lv_refr_now(display);
  if (presented != NULL) {
    rc = presented(presented_user);
    if (rc != H2_PAL_OK) goto done;
  }
  const char* source =
      config->alipay == NULL                                   ? "controlled"
      : config->alipay->source == H2_ALIPAY_SOURCE_VENDOR_SDK  ? "vendor_sdk"
      : config->alipay->source == H2_ALIPAY_SOURCE_INDEPENDENT ? "independent"
                                                               : "controlled";
  printf("H2_ALIPAY_PAYMENT_READY mode=menu source=%s private_payloads=0\n",
         source);
  p->last_tick = now_ms(p);
  while (true) {
    uint64_t now = now_ms(p);
    input_status(p, now);
    if (p->mode == CODE && p->model.length != 0u &&
        now >= p->model.refresh_ms) {
      clear_qr(p);
      wipe(p->model.visible, sizeof(p->model.visible));
      p->model.length = 0u;
      status(p, "Refreshing payment code...");
    }
    lv_tick_inc((uint32_t)(now - p->last_tick));
    p->last_tick = now;
    (void)lv_timer_handler();
    if (config->should_exit != NULL && config->should_exit(config->user)) {
      p->exiting = true;
      p->leaving = true;
      ++p->generation;
    }
    if (p->worker != NULL && h2_atomic_bool_load(&p->done, H2_ATOMIC_ACQUIRE)) {
      if (h2_pal_task_join(rt->task, p->worker) == 0) {
        p->worker = NULL;
        finish_job(p);
      }
    }
    if (p->exiting && p->worker == NULL && !p->tap_active && !p->scanning &&
        p->advertising == NULL)
      break;
    uint8_t bytes[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    h2_runtime_event_t event = {.payload = bytes,
                                .payload_capacity = sizeof(bytes)};
    int polled;
    while ((polled = h2_runtime_poll_event(rt, &event)) == 0) {
      button_event(p, &event);
      if (event.kind == H2_RUNTIME_SYSTEM_EVENT_TIME_ADJUSTED &&
          p->mode == CODE && p->model.length != 0u && p->code_wall_ms != 0u) {
        uint64_t wall = 0u;
        if (h2_pal_time_get_wall_ms(rt->time, &wall) == 0) {
          uint64_t elapsed = now_ms(p) - p->code_mono_ms;
          uint64_t expected = p->code_wall_ms <= UINT64_MAX - elapsed
                                  ? p->code_wall_ms + elapsed
                                  : 0u;
          uint64_t delta = wall > expected ? wall - expected : expected - wall;
          /* Routine small corrections must not rotate the QR on every
           * result query. Refresh after a material clock step. */
          if (delta > 5000u) p->model.refresh_ms = 0u;
        }
      }
      if (p->mode == TAP && !p->leaving && p->worker == NULL &&
          p->query_due == 0u &&
          event.kind == H2_RUNTIME_COMPONENT_EVENT_NFC_STATE &&
          event.component_id == config->payment_nfc_component_id) {
        h2_runtime_nfc_state_t nfc;
        if (h2_runtime_component_state_nfc(rt, event.component_id, &nfc) == 0 &&
            nfc.status == H2_RUNTIME_NFC_STATE_DISCOVERED && nfc.uid_len > 0u &&
            nfc.uid_len <= sizeof(p->uid)) {
          memcpy(p->uid, nfc.uid, nfc.uid_len);
          p->uid_length = nfc.uid_len;
          (void)start_job(p, J_FRAME);
        }
      }
    }
    if (polled != H2_PAL_ERR_WOULD_BLOCK && polled != H2_PAL_ERR_TIMEOUT) {
      rc = polled;
      p->exiting = true;
      p->leaving = true;
      ++p->generation;
    }
    if (p->queue_full) {
      status(p, "Busy; try again");
      p->queue_full = false;
    }
    if (p->count != 0u &&
        (p->worker == NULL || p->actions[p->head] == A_BACK)) {
      action_t action = (action_t)p->actions[p->head];
      p->head = (p->head + 1u) % 8u;
      --p->count;
      if (action == A_BACK) {
        p->leaving = true;
        ++p->generation;
        status(p, "Finishing current operation...");
      } else if (!p->leaving && p->worker == NULL) {
        if (action == A_CODE && p->mode == MENU) {
          p->mode = CODE;
          ++p->generation;
          (void)start_job(p, J_CODE);
        } else if (action == A_TAP && p->mode == MENU &&
                   p->api->vtable->start_tap != NULL) {
          p->mode = TAP;
          ++p->generation;
          p->query_due = 0u;
          (void)start_job(p, J_TAP_START);
        } else if (action == A_REFRESH && p->mode == CODE)
          (void)start_job(p, J_CODE);
        else if (action == A_CONFIRM && p->mode == CONFIRM)
          (void)start_job(p, J_CONFIRM);
      }
    }
    if (p->stay_on_result && !p->leaving && p->worker == NULL) {
      (void)start_job(p, J_STOP);
    } else if (p->leaving && p->worker == NULL) {
      if (p->tap_active || p->scanning || p->advertising != NULL)
        (void)start_job(p, J_STOP);
      else {
        clear_qr(p);
        h2_alipay_payment_model_clear(&p->model);
        p->mode = MENU;
        p->leaving = false;
        if (p->exiting) break;
        status(p, "Choose payment method");
        controls(p);
      }
    } else if (p->worker == NULL && p->mode == CODE) {
      if (now >= p->model.refresh_ms)
        (void)start_job(p, J_CODE);
      else if (h2_alipay_payment_model_query(&p->model, now, &p->query) == 0)
        (void)start_job(p, J_QUERY);
    } else if (p->worker == NULL && p->mode == TAP && p->tap_active &&
               p->query_due == 0u) {
      h2_runtime_nfc_state_t nfc;
      if (h2_runtime_component_state_nfc(rt, config->payment_nfc_component_id,
                                         &nfc) == 0 &&
          nfc.status == H2_RUNTIME_NFC_STATE_DISCOVERED && nfc.uid_len > 0u &&
          nfc.uid_len <= sizeof(p->uid)) {
        memcpy(p->uid, nfc.uid, nfc.uid_len);
        p->uid_length = nfc.uid_len;
        (void)start_job(p, J_FRAME);
      }
    } else if (p->worker == NULL && p->mode == TAP && p->query_due != 0u) {
      if (now >= p->tap_deadline) {
        p->mode = RESULT;
        status(p, "Result unknown; check merchant");
        p->stay_on_result = true;
        (void)snprintf(p->terminal_message, sizeof(p->terminal_message), "%s",
                       "Result unknown; check merchant");
        controls(p);
      } else {
        bool received = false;
        if (!h2_atomic_flag_test_and_set(&p->ble_lock, H2_ATOMIC_ACQUIRE)) {
          if (p->ble_pending) {
            memcpy(p->job_ble, p->ble_mailbox, 27u);
            wipe(p->ble_mailbox, 27u);
            p->ble_pending = false;
            received = true;
          }
          h2_atomic_flag_clear(&p->ble_lock, H2_ATOMIC_RELEASE);
        }
        if (received)
          (void)start_job(p, J_BLE_RESULT);
        else if (now >= p->query_due) {
          p->query_due = now + 2000u;
          (void)start_job(p, J_TAP_QUERY);
        }
      }
    }
    int waited = h2_runtime_wait_notify(rt, 20u);
    if (waited != 0 && waited != H2_PAL_ERR_TIMEOUT) {
      rc = waited;
      p->exiting = true;
      p->leaving = true;
      ++p->generation;
    }
  }
done:
  if (p->touch != NULL) h2_lvgl_touch_destroy(p->touch);
  if (p->button_group != NULL) lv_group_delete(p->button_group);
  clear_qr(p);
  lv_obj_clean(lv_screen_active());
  if (flag_open) h2_atomic_flag_destroy(&p->ble_lock);
  if (atomic_open) h2_atomic_bool_destroy(&p->done);
  wipe(p, sizeof(*p));
  h2_pal_mem_free(rt->mem, p);
  return rc;
}
