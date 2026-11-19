#include "h2_alipay_payment_page.h"
#include "h2_alipay_payment_model.h"
#include "h2_alipay_tap.h"
#include "h2_atomic.h"
#include "h2_qrcode.h"
#include "h2_sha256.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

typedef enum mode {
  MENU,
  CODE,
  TAP,
  RESULT = 4,
  UNBIND_CONFIRM,
  UNBIND_DONE
} payment_mode_t;
typedef enum action {
  A_CODE = 1,
  A_TAP,
  A_REFRESH,
  A_BACK = 5,
  A_UNBIND,
  A_CONFIRM_UNBIND,
  A_REBIND
} action_t;
typedef enum button {
  B_CODE,
  B_TAP,
  B_REFRESH,
  B_BACK,
  B_UNBIND,
  B_CONFIRM_UNBIND,
  B_COUNT
} button_t;
typedef enum job {
  J_NONE,
  J_CODE,
  J_QUERY,
  J_TAP_START,
  J_FRAME,
  J_TAP_QUERY = 6,
  J_STOP = 9,
  J_UNBIND
} job_t;
typedef struct payment_page {
  h2_runtime_t* rt;
  const h2_alipay_e2e_config_t* config;
  const h2_alipay_payment_api_t* api;
  lv_display_t* display;
  lv_group_t* button_group;
  bool buttons_enabled;
  lv_obj_t *title, *status, *qr, *buttons[B_COUNT];
  uint16_t* qr_pixels;
  int width, height;
  payment_mode_t mode;
  unsigned actions[8], head, count;
  bool queue_full, leaving, exiting, tap_active, stay_on_result;
  bool nfc_received, unbound;
  char terminal_message[96];
  int (*presented)(void*);
  void* presented_user;
  job_t job;
  h2_pal_task_t* worker;
  h2_atomic_bool_t done;
  uint8_t uid[H2_PAL_NFC_UID_MAX_LEN], uid_length;
  bool nfc_private_write, nfc_reply_started;
  const h2_pal_nfc_data_read_t* nfc_prepared;
  h2_alipay_tap_transport_t tap_transport;
  uint8_t generated[H2_ALIPAY_PAYMENT_CODE_CAPACITY];
  uint32_t generated_length, generation, job_generation;
  int call_rc, sdk_rc, nfc_read_rc;
  h2_alipay_payment_result_t result;
  h2_alipay_payment_model_t model;
  h2_alipay_payment_attempt_t query;
  uint64_t tap_deadline, query_due, last_tick;
  uint64_t code_wall_ms, code_mono_ms, diagnostic_ms;
  unsigned key_events, query_sequence;
} payment_page_t;

static void log_payment(payment_page_t* page, const char* format, ...) {
  char message[512];
  va_list args;
  va_start(args, format);
  (void)vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  (void)h2_pal_log_write(page->rt->log, H2_PAL_LOG_INFO, "alipay/payment",
                         message);
}

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
static void back_clicked(lv_event_t* e) {
  enqueue(lv_event_get_user_data(e), A_BACK);
}
static void unbind_clicked(lv_event_t* e) {
  enqueue(lv_event_get_user_data(e), A_UNBIND);
}
static void confirm_unbind_clicked(lv_event_t* e) {
  payment_page_t* p = lv_event_get_user_data(e);
  enqueue(p, p->mode == UNBIND_DONE ? A_REBIND : A_CONFIRM_UNBIND);
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
                 p->mode == MENU ? p->height - 20
                 : (p->mode == UNBIND_CONFIRM || p->mode == UNBIND_DONE)
                     ? 52
                     : p->height - 88);
  for (unsigned i = 0u; i < B_COUNT; ++i)
    lv_obj_set_hidden(p->buttons[i], true);
  lv_label_set_text(lv_obj_get_child(p->buttons[B_BACK], 0),
                    p->mode == UNBIND_CONFIRM ? "Cancel" : "Back");
  lv_label_set_text(lv_obj_get_child(p->buttons[B_CONFIRM_UNBIND], 0),
                    p->mode == UNBIND_DONE ? "Continue" : "Confirm");
  int right_button_x = p->width / 2 + 4;
  lv_obj_set_x(p->buttons[B_BACK],
               p->mode == UNBIND_CONFIRM ? 8 : right_button_x);
  if (p->mode == MENU) {
    lv_obj_set_hidden(p->buttons[B_CODE], false);
    lv_obj_set_hidden(p->buttons[B_TAP], p->api->vtable->start_tap == NULL);
    lv_obj_set_hidden(p->buttons[B_UNBIND], p->api->vtable->unbind == NULL);
  } else {
    lv_obj_set_hidden(p->buttons[B_BACK], p->mode == UNBIND_DONE);
    if (p->mode == CODE) lv_obj_set_hidden(p->buttons[B_REFRESH], false);
    if (p->mode == UNBIND_CONFIRM || p->mode == UNBIND_DONE)
      lv_obj_set_hidden(p->buttons[B_CONFIRM_UNBIND], false);
  }
  for (unsigned i = 0u; i < B_COUNT; ++i) {
    bool disabled =
        i == B_BACK ? p->job == J_UNBIND : p->worker != NULL || p->leaving;
    if (disabled)
      lv_obj_add_state(p->buttons[i], LV_STATE_DISABLED);
    else
      lv_obj_remove_state(p->buttons[i], LV_STATE_DISABLED);
  }
  if (p->button_group != NULL) {
    lv_obj_t* focused = lv_group_get_focused(p->button_group);
    if (focused == NULL || lv_obj_is_hidden(focused) ||
        lv_obj_has_state(focused, LV_STATE_DISABLED)) {
      for (unsigned i = 0u; i < B_COUNT; ++i)
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
  for (size_t i = 0u; i < 4u; ++i) {
    if (ids[i] == 0u) return H2_PAL_ERR_INVALID_ARG;
    h2_runtime_component_info_t info;
    int rc = h2_runtime_component_get(p->rt, ids[i], &info);
    if (rc != 0) return rc;
    if (info.kind != H2_RUNTIME_COMPONENT_BUTTON) return H2_PAL_ERR_INVALID_ARG;
    for (size_t j = 0u; j < i; ++j)
      if (ids[i] == ids[j]) return H2_PAL_ERR_INVALID_ARG;
  }
  p->button_group = lv_group_create();
  if (p->button_group == NULL) return H2_PAL_ERR_NO_MEMORY;
  p->buttons_enabled = true;
  for (unsigned i = 0u; i < B_COUNT; ++i)
    lv_group_add_obj(p->button_group, p->buttons[i]);
  return H2_PAL_OK;
}
static int focused_index(payment_page_t* p) {
  if (p->button_group == NULL) return -1;
  lv_obj_t* focused = lv_group_get_focused(p->button_group);
  for (unsigned i = 0u; i < B_COUNT; ++i)
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
    if (p->mode != MENU && p->job != J_UNBIND) enqueue(p, A_BACK);
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
  lv_obj_set_clickable(p->qr, true);
  lv_obj_add_event_cb(p->qr, refresh_clicked, LV_EVENT_CLICKED, p);
  lv_obj_set_pos(p->qr, (p->width - span) / 2, 32);
  return 0;
}

static int select_nfc_profile(void* user, const uint8_t* uid,
                              uint8_t uid_length, const uint8_t header[16],
                              h2_pal_nfc_reader_options_t* options) {
  payment_page_t* page = user;
  h2_alipay_nfc_capabilities_t capabilities;
  int rc =
      h2_alipay_nfc_capabilities(uid, uid_length, header, 16u, &capabilities);
  if (rc != H2_PAL_OK) return rc;
  page->nfc_private_write =
      page->config->payment_nfc_fast_write && capabilities.private_write;
  options->fast_read =
      page->config->payment_nfc_fast_read && capabilities.fast_read;
  options->prepare_ndef_write = !page->nfc_private_write;
  log_payment(
      page,
      "H2_ALIPAY_NFC_CAPABILITIES manufacturer=0x%02x internal_id=0x%02x fast_read=%u fast_write=%u",
      header[0], header[9], (unsigned)options->fast_read,
      (unsigned)page->nfc_private_write);
  return H2_PAL_OK;
}
static int write_private_reply(payment_page_t* page, const uint8_t* data,
                               size_t length) {
  const h2_pal_nfc_reader_session_api_t* api =
      page->config->payment_nfc_session;
  if (api == NULL || api->exchange == NULL || length == 0u ||
      length > H2_ALIPAY_NFC_REPLY_MAX)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_periph_id_t id;
  int rc = h2_runtime_periph_id(page->rt,
                                page->config->payment_nfc_component_id, &id);
  uint8_t command[H2_ALIPAY_NFC_FRAME_MAX], reply[3];
  size_t frames = (length + H2_ALIPAY_NFC_FRAME_DATA_MAX - 1u) /
                  H2_ALIPAY_NFC_FRAME_DATA_MAX;
  for (size_t frame = 1u; rc == H2_PAL_OK && frame <= frames; ++frame) {
    size_t command_length = 0u;
    rc = h2_alipay_nfc_private_frame(data, length, frame, command,
                                     sizeof(command), &command_length);
    h2_pal_nfc_reader_reply_t result = {0};
    unsigned waits = 0u;
    while (rc == H2_PAL_OK) {
      memset(reply, 0, sizeof(reply));
      rc = api->exchange(api->user, id, page->nfc_prepared, command,
                         command_length, 25u, reply, sizeof(reply), &result);
      if (rc != H2_PAL_ERR_BUSY || waits++ >= 25u) break;
      rc = h2_pal_time_sleep_ms(page->rt->time, 5u);
    }
    if (rc == H2_PAL_OK)
      rc = h2_alipay_nfc_private_ack(reply, result.bits, result.crc_valid);
    log_payment(
        page,
        "H2_ALIPAY_NFC_PRIVATE_WRITE frame=%zu frames=%zu sdk_bytes=%zu bytes=%zu bits=%zu crc_valid=%u ack=0x%02x pal_rc=%d",
        frame, frames, length, command_length, result.bits,
        (unsigned)result.crc_valid, result.bits >= 8u ? reply[0] : 0u, rc);
    /* A timeout or invalid ACK can follow actual receipt. Never resend or
     * switch to ordinary NDEF after any attempted private frame. */
  }
  wipe(command, sizeof(command));
  wipe(reply, sizeof(reply));
  return rc;
}

static int write_reply(void* user, const uint8_t* data, size_t length) {
  payment_page_t* page = user;
  if (page->nfc_prepared == NULL || page->nfc_reply_started)
    return H2_PAL_ERR_INVALID_STATE;
  page->nfc_reply_started = true;
  if (page->nfc_private_write) return write_private_reply(page, data, length);
  if (page->config->payment_nfc_encode_reply == NULL)
    return H2_PAL_ERR_UNSUPPORTED;
  uint8_t message[512];
  size_t size = 0u;
  int rc = page->config->payment_nfc_encode_reply(
      page->config->user, data, length, message, sizeof(message), &size);
  if (rc == H2_PAL_OK && (size == 0u || size > sizeof(message)))
    rc = H2_PAL_ERR_FORMAT;
  h2_pal_periph_id_t id = 0u;
  if (rc == H2_PAL_OK)
    rc = h2_runtime_periph_id(page->rt, page->config->payment_nfc_component_id,
                              &id);
  unsigned busy_waits = 0u;
  while (rc == H2_PAL_OK) {
    rc = h2_pal_nfc_finish_ndef_write(page->rt->nfc, id, page->nfc_prepared,
                                      message, size);
    if (rc != H2_PAL_ERR_BUSY) page->nfc_prepared = NULL;
    if (rc != H2_PAL_ERR_BUSY || busy_waits == 100u) break;
    if (busy_waits == 0u)
      (void)h2_pal_log_write(
          page->rt->log, H2_PAL_LOG_WARN, "alipay/payment",
          "H2_ALIPAY_NFC_WRITE phase=wait pal_rc=-18 limit=100 delay_ms=20");
    ++busy_waits;
    rc = h2_pal_time_sleep_ms(page->rt->time, 20u);
  }
  char diagnostic[160];
  (void)snprintf(diagnostic, sizeof(diagnostic),
                 "H2_ALIPAY_NFC_WRITE phase=complete pal_rc=%d sdk_bytes=%zu "
                 "ndef_bytes=%zu busy_waits=%u",
                 rc, length, size, busy_waits);
  (void)h2_pal_log_write(page->rt->log, H2_PAL_LOG_WARN, "alipay/payment",
                         diagnostic);
  wipe(message, sizeof(message));
  return rc;
}
static int read_ndef_frame(payment_page_t* page, h2_pal_periph_id_t id,
                           h2_pal_nfc_data_read_t* data) {
  unsigned attempt = 1u, busy_waits = 0u;
  int last_read_rc = H2_PAL_ERR_BUSY;
  while (attempt <= 3u) {
    const h2_pal_nfc_reader_session_api_t* session =
        page->config->payment_nfc_session;
    int rc =
        session != NULL
            ? session->prepare(session->user, id, page->uid, page->uid_length,
                               select_nfc_profile, page, page->rt->mem, data)
            : h2_pal_nfc_prepare_ndef_write(page->rt->nfc, id, page->uid,
                                            page->uid_length, page->rt->mem,
                                            data);
    if (rc != H2_PAL_ERR_BUSY)
      log_payment(page,
                  "H2_ALIPAY_NFC_READ attempt=%u limit=3 pal_rc=%d bytes=%zu "
                  "busy_waits=%u\n",
                  attempt, rc, rc == 0 ? data->len : 0u, busy_waits);
    if (rc == H2_PAL_OK) return rc;
    if (data->bytes != NULL) {
      wipe(data->bytes, data->len);
      h2_pal_mem_free(page->rt->mem, data->bytes);
    }
    *data = (h2_pal_nfc_data_read_t){0};
    if (rc == H2_PAL_ERR_BUSY) {
      if (busy_waits == 100u) {
        log_payment(
            page,
            "H2_ALIPAY_NFC_BUSY phase=stop waits=%u limit=100 result=%d\n",
            busy_waits, last_read_rc);
        return last_read_rc;
      }
      ++busy_waits;
      if (busy_waits == 1u)
        log_payment(
            page,
            "H2_ALIPAY_NFC_BUSY phase=wait waits=%u limit=100 prior_rc=%d\n",
            busy_waits, last_read_rc);
    } else {
      last_read_rc = rc;
      if (attempt == 3u || (rc != H2_PAL_ERR_IO && rc != H2_PAL_ERR_TIMEOUT &&
                            rc != H2_PAL_ERR_NOT_FOUND))
        return rc;
      ++attempt;
    }
    int waited = h2_pal_time_sleep_ms(page->rt->time, 20u);
    if (waited != H2_PAL_OK) return waited;
  }
  return last_read_rc;
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
      if (api->start_tap == NULL ||
          p->config->payment_nfc_encode_reply == NULL) {
        p->call_rc = H2_PAL_ERR_UNSUPPORTED;
        break;
      }
      p->tap_transport =
          (h2_alipay_tap_transport_t){.user = p, .write_reply = write_reply};
      p->call_rc = api->start_tap(p->api->user, &p->tap_transport, &p->result);
      break;
    }
    case J_FRAME: {
      h2_pal_periph_id_t id = 0u;
      p->nfc_private_write = false;
      p->nfc_reply_started = false;
      h2_pal_nfc_data_read_t data = {0};
      p->call_rc =
          h2_runtime_periph_id(p->rt, p->config->payment_nfc_component_id, &id);
      if (p->call_rc == 0) p->call_rc = read_ndef_frame(p, id, &data);
      p->nfc_read_rc = p->call_rc;
      if (p->call_rc == 0) {
        h2_sha256_t sha;
        uint8_t digest[32];
        h2_sha256_init(&sha);
        h2_sha256_update(&sha, data.bytes, data.len);
        h2_sha256_finish(&sha, digest);
        p->nfc_prepared = &data;
        p->call_rc =
            api->receive_ndef(p->api->user, data.bytes, data.len, &p->result);
        if (p->nfc_prepared != NULL) {
          int canceled;
          do {
            canceled = h2_pal_nfc_finish_ndef_write(p->rt->nfc, id,
                                                    p->nfc_prepared, NULL, 0u);
            if (canceled == H2_PAL_ERR_BUSY)
              (void)h2_pal_time_sleep_ms(p->rt->time, 20u);
          } while (canceled == H2_PAL_ERR_BUSY);
          p->nfc_prepared = NULL;
        }
        log_payment(
            p,
            "H2_ALIPAY_NFC_CALLBACKS notify_payment_result_handle=%u "
            "processing_status=%d processing_error=%d send_data_handle=%u "
            "write_rc=%d response_sent=%u",
            (unsigned)p->result.processing_notified,
            p->result.processing_status, p->result.processing_error,
            (unsigned)p->result.write_attempted, p->result.nfc_write_rc,
            (unsigned)p->result.response_sent);
        char hex[65], diagnostic[160];
        h2_sha256_hex(digest, hex);
        (void)snprintf(
            diagnostic, sizeof(diagnostic),
            "H2_ALIPAY_NFC_INPUT bytes=%zu sha256=%s sdk_rc=%d transport_rc=%d",
            data.len, hex, p->result.sdk_rc, p->result.transport_rc);
        (void)h2_pal_log_write(p->rt->log, H2_PAL_LOG_WARN, "alipay/payment",
                               diagnostic);
      }
      if (data.bytes != NULL) {
        wipe(data.bytes, data.len);
        h2_pal_mem_free(p->rt->mem, data.bytes);
      }
      break;
    }
    case J_TAP_QUERY:
      p->call_rc = api->query_tap(p->api->user, &p->result);
      break;
    case J_UNBIND:
      p->call_rc = api->unbind != NULL ? api->unbind(p->api->user, &p->sdk_rc)
                                       : H2_PAL_ERR_INVALID_STATE;
      break;
    case J_STOP:
      p->call_rc = api->stop_tap(p->api->user);
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
  if (job == J_QUERY || job == J_TAP_QUERY) {
    char diagnostic[160];
    ++p->query_sequence;
    (void)snprintf(
        diagnostic, sizeof(diagnostic),
        "H2_ALIPAY_PAYMENT_QUERY phase=begin operation=%u attempt=%u generation=%u sequence=%u",
        (unsigned)job, (unsigned)p->query.id, (unsigned)p->generation,
        p->query_sequence);
    (void)h2_pal_log_write(p->rt->log, H2_PAL_LOG_INFO, "alipay/payment",
                           diagnostic);
    status(p, "Checking payment result...");
  }
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
  p->stay_on_result = p->tap_active;
  h2_alipay_payment_model_clear(&p->model);
}
static void finish_job(payment_page_t* p) {
  job_t job = p->job;
  p->job = J_NONE;
  if (job == J_TAP_START && p->call_rc == 0) p->tap_active = true;
  log_payment(
      p,
      "H2_ALIPAY_PAYMENT operation=%u pal_rc=%d sdk_rc=%d sdk_status=%d state=%d nfc_rc=%d sent=%d nfc_attempted=%d processing_status=%d processing_error=%d transport_rc=%d code_length=%u",
      (unsigned)job, p->call_rc,
      (job == J_CODE || job == J_UNBIND) ? p->sdk_rc : p->result.sdk_rc,
      p->result.sdk_status, p->result.state, p->result.nfc_write_rc,
      p->result.response_sent, p->result.write_attempted,
      p->result.processing_status, p->result.processing_error,
      p->result.transport_rc,
      job == J_CODE ? (unsigned)p->generated_length : 0u);
  if (job == J_UNBIND) {
    if (p->call_rc == H2_PAL_OK && p->sdk_rc == 0) {
      p->unbound = true;
      clear_qr(p);
      h2_alipay_payment_model_clear(&p->model);
      p->mode = UNBIND_DONE;
      status(
          p,
          "Device unbound locally.\nCheck Alipay > Smart Devices\non your phone and unbind\nthere if still linked.");
    } else {
      p->mode = RESULT;
      char text[96];
      (void)snprintf(text, sizeof(text), "Unbind failed\nSDK: %d / rc: %d",
                     p->sdk_rc, p->call_rc);
      status(p, text);
    }
  } else if (job == J_STOP) {
    if (p->call_rc == 0) {
      p->tap_active = false;
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
      status(p, "Stopping NFC session; waiting for cleanup");
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
    } else if (job == J_FRAME) {
      p->nfc_received = p->call_rc == 0 || p->result.response_sent;
      if (p->nfc_read_rc != 0) {
        char text[80];
        (void)snprintf(text, sizeof(text), "NFC read failed\nrc: %d",
                       p->nfc_read_rc);
        p->mode = RESULT;
        status(p, text);
      } else if (p->result.write_attempted && p->result.nfc_write_rc != 0) {
        char text[80];
        (void)snprintf(text, sizeof(text), "NFC write failed\nrc: %d",
                       p->result.nfc_write_rc);
        p->mode = RESULT;
        status(p, text);
      } else if (p->call_rc == 0 && p->result.response_sent) {
        p->mode = TAP;
        p->query_due = now_ms(p) + 2000u;
        status(p, "Response sent; checking payment");
      } else {
        p->mode = RESULT;
        status(p, "Tap not completed; check merchant");
      }
    } else if (job == J_QUERY || job == J_TAP_QUERY) {
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
      } else if (belongs) {
        if (p->call_rc != 0 || p->result.sdk_rc != 0) {
          char text[96];
          (void)snprintf(text, sizeof(text),
                         "Payment query failed\nSDK: %d / rc: %d",
                         p->result.sdk_rc, p->call_rc);
          status(p, text);
        } else if (p->result.state == H2_ALIPAY_PAYMENT_PENDING)
          status(p, "Waiting for merchant result");
        else
          status(p, "Payment result unknown; retrying");
      }
    }
  }
  if (p->mode == RESULT && !p->leaving && p->tap_active) {
    if (!p->stay_on_result)
      (void)snprintf(p->terminal_message, sizeof(p->terminal_message), "%s",
                     lv_label_get_text(p->status));
    p->stay_on_result = true;
  }
  wipe(p->generated, sizeof(p->generated));
  p->generated_length = 0u;
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
  bool atomic_open = false;
  int rc = 0;
  if (h2_atomic_bool_init(&p->done, false) != H2_ATOMIC_OK) {
    rc = H2_PAL_ERR_NO_MEMORY;
    goto done;
  }
  atomic_open = true;
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
  p->buttons[B_CODE] =
      make_button(p, "Pay code", 20, 62, width - 40, code_clicked);
  p->buttons[B_TAP] =
      make_button(p, "Tap to pay", 20, 118, width - 40, tap_clicked);
  p->buttons[B_REFRESH] = make_button(p, "Refresh", 8, height - 44,
                                      (width - 24) / 2, refresh_clicked);
  p->buttons[B_BACK] = make_button(p, "Back", width / 2 + 4, height - 44,
                                   (width - 24) / 2, back_clicked);
  p->buttons[B_UNBIND] =
      make_button(p, "Unbind", 20, 174, width - 40, unbind_clicked);
  p->buttons[B_CONFIRM_UNBIND] =
      make_button(p, "Confirm", width / 2 + 4, height - 44, (width - 24) / 2,
                  confirm_unbind_clicked);
  for (unsigned i = 0u; i < B_COUNT; ++i)
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
  const char* source = config->alipay == NULL ? "controlled"
                       : config->alipay->source == H2_ALIPAY_SOURCE_INDEPENDENT
                           ? "independent"
                           : "controlled";
  log_payment(
      p, "H2_ALIPAY_PAYMENT_READY mode=menu source=%s private_payloads=0\n",
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
    if (p->unbound && p->exiting && p->worker == NULL && !p->tap_active) {
      rc = H2_ALIPAY_PAYMENT_PAGE_UNBOUND;
      break;
    }
    if (p->exiting && p->worker == NULL && !p->tap_active) break;
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
      if (p->mode == TAP && p->tap_active && !p->leaving && p->worker == NULL &&
          p->query_due == 0u && !p->nfc_received &&
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
    if (p->worker == NULL && p->tap_active && p->mode == TAP &&
        now >= p->tap_deadline) {
      p->mode = RESULT;
      p->stay_on_result = true;
      (void)snprintf(p->terminal_message, sizeof(p->terminal_message), "%s",
                     "Result unknown; check merchant");
      status(p, p->terminal_message);
    }
    if (p->count != 0u &&
        (p->worker == NULL || p->actions[p->head] == A_BACK)) {
      action_t action = (action_t)p->actions[p->head];
      p->head = (p->head + 1u) % 8u;
      --p->count;
      if (p->unbound && (action == A_REBIND || action == A_BACK)) {
        rc = H2_ALIPAY_PAYMENT_PAGE_UNBOUND;
        break;
      }
      if (action == A_BACK && p->job != J_UNBIND) {
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
          p->nfc_received = false;
          if (start_job(p, J_TAP_START) != 0) {
            p->mode = RESULT;
            controls(p);
          }
        } else if (action == A_UNBIND && p->mode == MENU &&
                   p->api->vtable->unbind != NULL && !p->tap_active) {
          p->mode = UNBIND_CONFIRM;
          ++p->generation;
          status(p, "Unbind this device?\nYou will need to bind again.");
          controls(p);
          lv_group_focus_obj(p->buttons[B_BACK]);
        } else if (action == A_CONFIRM_UNBIND && p->mode == UNBIND_CONFIRM &&
                   p->api->vtable->unbind != NULL && !p->tap_active) {
          /* Do not carry queued clicks into a new account session. */
          p->head = p->count = 0u;
          ++p->generation;
          status(p, "Unbinding device...");
          (void)start_job(p, J_UNBIND);
        } else if (action == A_REFRESH && p->mode == CODE)
          (void)start_job(p, J_CODE);
      }
    }
    now = now_ms(p);
    if (p->stay_on_result && !p->leaving && p->worker == NULL) {
      (void)start_job(p, J_STOP);
    } else if (p->leaving && p->worker == NULL) {
      if (p->tap_active)
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
               p->query_due == 0u && !p->nfc_received) {
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
        if (now >= p->query_due) {
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
  if (p->button_group != NULL) lv_group_delete(p->button_group);
  clear_qr(p);
  lv_obj_clean(lv_screen_active());
  if (atomic_open) h2_atomic_bool_destroy(&p->done);
  wipe(p, sizeof(*p));
  h2_pal_mem_free(rt->mem, p);
  return rc;
}
