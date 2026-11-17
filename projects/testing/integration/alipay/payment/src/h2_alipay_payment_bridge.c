#include "h2_alipay_payment_bridge.h"
#include "h2_alipay_api.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

static h2_alipay_tap_transport_t s_transport;
static h2_alipay_payment_result_t s_tap;
static bool s_tap_active;
static bool s_received_ndef, s_notify_failed;
static uint8_t s_notify_index, s_notify_count;
static const h2_alipay_payment_bridge_t* s_owner;
static struct {
  uint16_t manufacturer;
  uint8_t data[100], uuid[16];
  size_t length, uuid_length;
  bool valid, notify, first;
} s_pending;

static int enter(void* user) {
  const h2_alipay_payment_bridge_t* bridge = user;
  if (bridge == NULL || bridge->enter == NULL || bridge->leave == NULL)
    return H2_PAL_ERR_INVALID_STATE;
  int rc = bridge->enter(bridge->user);
  if (rc == H2_PAL_OK && s_tap_active && s_owner != bridge)
    return bridge->leave(bridge->user, H2_PAL_ERR_BUSY);
  return rc;
}

static int leave(void* user, int result) {
  const h2_alipay_payment_bridge_t* bridge = user;
  return bridge->leave(bridge->user, result);
}

static void record(void* user, const char* operation, int result,
                   uint32_t item) {
  const h2_alipay_payment_bridge_t* bridge = user;
  if (bridge->record != NULL)
    bridge->record(bridge->user, operation, result, item);
}

static void wipe(void* data, size_t size) {
  volatile uint8_t* p = data;
  while (size-- != 0u) *p++ = 0u;
}

static void state(h2_alipay_payment_result_t* out) {
  out->state = H2_ALIPAY_PAYMENT_UNKNOWN;
  if (out->sdk_rc != ALIPAY_RV_OK) return;
  switch (out->sdk_status) {
    case ALIPAY_PAYMENT_STATUS_SUCCESS:
      out->state = H2_ALIPAY_PAYMENT_PAID;
      break;
    case ALIPAY_PAYMENT_STATUS_NOTHING:
    case ALIPAY_PAYMENT_STATUS_PROCESSING:
      out->state = H2_ALIPAY_PAYMENT_PENDING;
      break;
    case ALIPAY_PAYMENT_STATUS_FAIL:
      out->state = H2_ALIPAY_PAYMENT_REJECTED;
      break;
    case ALIPAY_PAYMENT_STATUS_UNBIND_BY_USER:
      out->state = H2_ALIPAY_PAYMENT_UNBOUND;
      break;
    case ALIPAY_PAYMENT_STATUS_DISABLED_BY_USER:
      out->state = H2_ALIPAY_PAYMENT_DISABLED;
      break;
    default:
      break;
  }
}

static int get_code(void* user, uint8_t* code, uint32_t* length, int* sdk_rc) {
  if (code == NULL || length == NULL || sdk_rc == NULL || *length < 20u)
    return H2_PAL_ERR_INVALID_ARG;
  uint32_t capacity = *length;
  *sdk_rc = INT_MIN;
  int rc = enter(user);
  if (rc != H2_PAL_OK) {
    wipe(code, capacity);
    *length = 0u;
    return rc;
  }
  *sdk_rc = alipay_get_paycode(code, length);
  if (*sdk_rc != ALIPAY_RV_OK) {
    wipe(code, capacity);
    *length = 0u;
  } else if ((*length != 18u && *length != 19u) || *length >= capacity)
    rc = H2_PAL_ERR_FORMAT;
  else {
    for (uint32_t i = 0u; i < *length; ++i)
      if (code[i] < '0' || code[i] > '9') rc = H2_PAL_ERR_FORMAT;
  }
  if (rc == H2_PAL_OK && *sdk_rc == 0)
    code[*length] = '\0';
  else {
    wipe(code, capacity);
    *length = 0u;
  }
  record(user, "alipay_get_paycode.return", *sdk_rc, *length);
  return leave(user, rc);
}

static int query_code(void* user, const char* code,
                      h2_alipay_payment_result_t* out) {
  if (code == NULL || out == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out = (h2_alipay_payment_result_t){
      .sdk_rc = INT_MIN, .sdk_status = ALIPAY_PAYMENT_STATUS_UNKNOWN};
  size_t length = 0u;
  while (length < 20u && code[length] >= '0' && code[length] <= '9') ++length;
  if ((length != 18u && length != 19u) || code[length] != '\0')
    return H2_PAL_ERR_INVALID_ARG;
  int rc = enter(user);
  if (rc != H2_PAL_OK) return rc;
  char copy[20], ext[256];
  memcpy(copy, code, length + 1u);
  uint32_t ext_length = sizeof(ext);
  alipay_payment_status_e status = ALIPAY_PAYMENT_STATUS_UNKNOWN;
  out->sdk_rc = alipay_query_order_result_string(copy, &status, out->amount,
                                                 ext, &ext_length);
  out->sdk_status = (int)status;
  out->amount[sizeof(out->amount) - 1u] = '\0';
  state(out);
  if (out->state != H2_ALIPAY_PAYMENT_PAID)
    wipe(out->amount, sizeof(out->amount));
  record(user, "alipay_query_order.return", out->sdk_rc, (uint32_t)status);
  wipe(copy, sizeof(copy));
  wipe(ext, sizeof(ext));
  return leave(user, H2_PAL_OK);
}

static int send_reply(const uint8_t* data, uint32_t length) {
  if (!s_tap_active) return H2_PAL_ERR_CLOSED;
  if (s_transport.write_reply == NULL) return H2_PAL_ERR_UNSUPPORTED;
  s_tap.write_attempted = true;
  s_tap.nfc_write_rc =
      s_transport.write_reply != NULL
          ? s_transport.write_reply(s_transport.user, data, length)
          : H2_PAL_ERR_UNSUPPORTED;
  s_tap.response_sent |= s_tap.nfc_write_rc == H2_PAL_OK;
  s_tap.transport_rc = s_tap.nfc_write_rc;
  return s_tap.nfc_write_rc;
}
static int broadcast_legacy(const uint8_t* data, uint32_t length) {
  if (!s_tap_active) return H2_PAL_ERR_CLOSED;
  s_tap.broadcast_attempted = true;
  s_tap.ble_send_rc =
      s_transport.broadcast_legacy != NULL
          ? s_transport.broadcast_legacy(s_transport.user, data, length)
          : H2_PAL_ERR_UNSUPPORTED;
  s_tap.transport_rc = s_tap.ble_send_rc;
  return s_tap.ble_send_rc;
}
static int notify(bool first, bool finish, const uint8_t* data,
                  uint32_t length) {
  (void)finish;
  if (!s_tap_active) return H2_PAL_ERR_CLOSED;
  if (data == NULL || length < 3u || length > sizeof(s_pending.data))
    return H2_PAL_ERR_FORMAT;
  uint8_t index = data[length - 2u], count = data[length - 1u];
  if (first) {
    s_notify_failed = false;
    s_notify_index = 0u;
    s_notify_count = count;
  }
  if (s_notify_failed) return s_tap.ble_send_rc;
  if (count == 0u || index >= count || index != s_notify_index ||
      count != s_notify_count || first != (index == 0u)) {
    s_notify_failed = true;
    return s_tap.ble_send_rc = s_tap.transport_rc = H2_PAL_ERR_FORMAT;
  }
  ++s_notify_index;
  if (s_transport.notify == NULL)
    return s_tap.transport_rc = H2_PAL_ERR_UNSUPPORTED;
  if (index + 1u == count) {
    wipe(&s_pending, sizeof(s_pending));
    memcpy(s_pending.data, data, length);
    s_pending.length = length;
    s_pending.notify = s_pending.valid = true;
    s_pending.first = first;
    s_tap.confirmation_required = true;
    return s_tap.transport_rc = H2_PAL_ERR_WOULD_BLOCK;
  }
  s_tap.ble_send_rc =
      s_transport.notify(s_transport.user, first, false, data, length);
  s_tap.transport_rc = s_tap.ble_send_rc;
  s_notify_failed = s_tap.ble_send_rc != H2_PAL_OK;
  return s_tap.ble_send_rc;
}
static int hex_digit(uint8_t c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* This delivery passes a 36-byte canonical UUID string to its callback.
 * PAL uses little-endian binary UUIDs; Bluetooth-base 16-bit UUIDs stay
 * compressed so the 20-byte payment data fits legacy advertising. */
static int uuid_bytes(const uint8_t* text, size_t length, uint8_t out[16],
                      size_t* size) {
  if (length == 2u || length == 16u) {
    memcpy(out, text, length);
    *size = length;
    return 0;
  }
  if (length != 36u) return H2_PAL_ERR_FORMAT;
  uint8_t big[16];
  size_t n = 0u;
  for (size_t i = 0u; i < length;) {
    if (i == 8u || i == 13u || i == 18u || i == 23u) {
      if (text[i++] != '-') return H2_PAL_ERR_FORMAT;
      continue;
    }
    int high = hex_digit(text[i]), low = hex_digit(text[i + 1u]);
    if (high < 0 || low < 0 || n >= sizeof(big)) return H2_PAL_ERR_FORMAT;
    big[n++] = (uint8_t)((high << 4) | low);
    i += 2u;
  }
  static const uint8_t base[12] = {0, 0,    0x10, 0,    0x80, 0,
                                   0, 0x80, 0x5f, 0x9b, 0x34, 0xfb};
  if (big[0] == 0u && big[1] == 0u &&
      memcmp(big + 4u, base, sizeof(base)) == 0) {
    out[0] = big[3];
    out[1] = big[2];
    *size = 2u;
  } else {
    for (size_t i = 0u; i < 16u; ++i) out[i] = big[15u - i];
    *size = 16u;
  }
  return 0;
}

static int broadcast_direct(int manufacturer, const uint8_t* data,
                            uint32_t length, const uint8_t* service,
                            uint32_t service_length, uint8_t quick) {
  if (!s_tap_active) return H2_PAL_ERR_CLOSED;
  if (s_tap.write_attempted) return s_tap.nfc_write_rc;
  s_tap.broadcast_attempted = true;
  uint8_t uuid[16];
  size_t uuid_length = 0u;
  if (manufacturer < 0 || manufacturer > UINT16_MAX || data == NULL ||
      length == 0u || length > 31u || service == NULL ||
      uuid_bytes(service, service_length, uuid, &uuid_length) != 0) {
    s_tap.ble_send_rc = s_tap.transport_rc = H2_PAL_ERR_FORMAT;
    return H2_PAL_ERR_FORMAT;
  }
  if (quick == 0u) {
    s_pending.manufacturer = (uint16_t)manufacturer;
    memcpy(s_pending.data, data, length);
    s_pending.length = length;
    memcpy(s_pending.uuid, uuid, uuid_length);
    s_pending.uuid_length = uuid_length;
    s_pending.valid = true;
    s_tap.confirmation_required = true;
    s_tap.ble_send_rc = s_tap.transport_rc = H2_PAL_ERR_WOULD_BLOCK;
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  s_tap.ble_send_rc = s_transport.broadcast_direct != NULL
                          ? s_transport.broadcast_direct(
                                s_transport.user, (uint16_t)manufacturer, data,
                                length, uuid, uuid_length, quick != 0u)
                          : H2_PAL_ERR_UNSUPPORTED;
  s_tap.response_sent |= s_tap.ble_send_rc == H2_PAL_OK;
  s_tap.transport_rc = s_tap.ble_send_rc;
  return s_tap.ble_send_rc;
}
static int processed(alipay_payment_status_t* status) {
  if (status == NULL || !s_tap_active) return H2_PAL_ERR_INVALID_STATE;
  s_tap.processing_status = (int)status->status;
  s_tap.processing_error = (int)status->code;
  /* Internal callback success is not a financial success result. */
  return H2_PAL_OK;
}
static int quick_changed(bool flag) {
  (void)flag;
  return H2_PAL_OK;
}

static int start_tap(void* user, const h2_alipay_tap_transport_t* transport,
                     h2_alipay_payment_result_t* out) {
  if (transport == NULL ||
      (transport->write_reply == NULL && transport->broadcast_direct == NULL &&
       transport->notify == NULL) ||
      out == NULL || transport->context < H2_ALIPAY_TAP_WALLET ||
      transport->context > H2_ALIPAY_TAP_OTHER)
    return H2_PAL_ERR_INVALID_ARG;
  int rc = enter(user);
  if (rc != H2_PAL_OK) return rc;
  if (s_tap_active) return leave(user, H2_PAL_ERR_INVALID_STATE);
  s_transport = *transport;
  wipe(&s_pending, sizeof(s_pending));
  s_tap =
      (h2_alipay_payment_result_t){.sdk_status = ALIPAY_PAYMENT_STATUS_UNKNOWN};
  s_tap_active = true;
  s_received_ndef = s_notify_failed = false;
  s_owner = user;
  if (transport->broadcast_direct != NULL) {
    s_tap.sdk_rc = alipay_npay_ble_broadcast_init();
    if (s_tap.sdk_rc != 0) {
      *out = s_tap;
      s_tap_active = false;
      s_owner = NULL;
      wipe(&s_transport, sizeof(s_transport));
      return leave(user, H2_PAL_ERR_IO);
    }
  }
  const alipay_nfc_payment_callback_t callbacks = {
      .send_data_handle = send_reply,
      .ble_broadcast_data_handle =
          transport->broadcast_legacy != NULL ? broadcast_legacy : NULL,
      .ble_notify_data_handle = transport->notify != NULL ? notify : NULL,
      .notify_payment_result_handle = processed,
      .ble_broadcast_direct_pay_data_handle =
          transport->broadcast_direct != NULL ? broadcast_direct : NULL,
      .quick_pay_flag_change_callback = quick_changed};
  alipay_nfc_init_callbacks(callbacks);
  *out = s_tap;
  return leave(user, H2_PAL_OK);
}
static int receive_ndef(void* user, const uint8_t* data, size_t length,
                        h2_alipay_payment_result_t* out) {
  if (data == NULL || length == 0u || length > UINT32_MAX || out == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  int rc = enter(user);
  if (rc != H2_PAL_OK) return rc;
  if (!s_tap_active || s_received_ndef || s_tap.write_attempted ||
      s_tap.response_sent || s_pending.valid)
    return leave(user, H2_PAL_ERR_INVALID_STATE);
  s_tap =
      (h2_alipay_payment_result_t){.sdk_status = ALIPAY_PAYMENT_STATUS_UNKNOWN};
  wipe(&s_pending, sizeof(s_pending));
  uint8_t amount[16] = {0};
  s_tap.sdk_rc = s_transport.context == H2_ALIPAY_TAP_WALLET
                     ? alipay_nfc_recv_data_handle(data, (uint32_t)length)
                     : alipay_nfc_recv_data_handle_with_context(
                           (alipay_nfc_pay_reader_context_t)s_transport.context,
                           data, (uint32_t)length, amount);
  wipe(amount, sizeof(amount));
  s_received_ndef = s_tap.sdk_rc == 0;
  s_tap.state = s_tap.response_sent ? H2_ALIPAY_PAYMENT_PENDING
                                    : H2_ALIPAY_PAYMENT_UNKNOWN;
  *out = s_tap;
  record(user, "alipay_nfc_receive.return", out->sdk_rc,
         (uint32_t)out->transport_rc);
  int result = out->response_sent ? H2_PAL_OK : out->transport_rc;
  if (!out->response_sent && !out->confirmation_required && result == H2_PAL_OK)
    result = H2_PAL_ERR_IO;
  return leave(user, result);
}
static int confirm_tap(void* user, h2_alipay_payment_result_t* out) {
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  int rc = enter(user);
  if (rc != H2_PAL_OK) return rc;
  if (!s_tap_active || !s_pending.valid ||
      (s_pending.notify ? s_transport.notify == NULL
                        : s_transport.broadcast_direct == NULL))
    return leave(user, H2_PAL_ERR_INVALID_STATE);
  /* Consume before sending: an ambiguous transport failure is never retried
   * by a second confirmation and cannot trigger a duplicate transmission. */
  s_pending.valid = false;
  s_tap.confirmation_required = false;
  s_tap.ble_send_rc =
      s_pending.notify
          ? s_transport.notify(s_transport.user, s_pending.first, true,
                               s_pending.data, s_pending.length)
          : s_transport.broadcast_direct(
                s_transport.user, s_pending.manufacturer, s_pending.data,
                s_pending.length, s_pending.uuid, s_pending.uuid_length, false);
  s_tap.transport_rc = s_tap.ble_send_rc;
  s_tap.response_sent |= s_tap.ble_send_rc == H2_PAL_OK;
  s_tap.state = s_tap.response_sent ? H2_ALIPAY_PAYMENT_PENDING
                                    : H2_ALIPAY_PAYMENT_UNKNOWN;
  wipe(&s_pending, sizeof(s_pending));
  *out = s_tap;
  return leave(user, s_tap.ble_send_rc);
}
static int receive_ble_result(void* user, const uint8_t* data, size_t length,
                              h2_alipay_payment_result_t* out) {
  if (data == NULL || length != 27u || out == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *out =
      (h2_alipay_payment_result_t){.sdk_status = ALIPAY_PAYMENT_STATUS_UNKNOWN};
  int rc = enter(user);
  if (rc != 0) return rc;
  if (!s_tap_active || !s_tap.response_sent)
    return leave(user, H2_PAL_ERR_INVALID_STATE);
  alipay_npay_ble_pay_result_t parsed = {0};
  out->sdk_rc = alipay_npay_ble_scan_pay_res_data_handle_v2(
      data, (uint32_t)length, &parsed);
  if (out->sdk_rc == 0 && parsed.pay_result_type <= 1u) {
    out->state = H2_ALIPAY_PAYMENT_PAID;
    out->sdk_status = ALIPAY_PAYMENT_STATUS_SUCCESS;
    if (parsed.pay_result_type == 1u)
      (void)snprintf(out->amount, sizeof(out->amount), "%u.%02u",
                     (unsigned)(parsed.real_pay_amount_fen / 100u),
                     (unsigned)(parsed.real_pay_amount_fen % 100u));
  }
  return leave(user, H2_PAL_OK);
}
static int query_tap(void* user, h2_alipay_payment_result_t* out) {
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out = (h2_alipay_payment_result_t){
      .sdk_rc = INT_MIN, .sdk_status = ALIPAY_PAYMENT_STATUS_UNKNOWN};
  int rc = enter(user);
  if (rc != H2_PAL_OK) return rc;
  if (!s_tap_active || !s_tap.response_sent)
    return leave(user, H2_PAL_ERR_INVALID_STATE);
  alipay_payment_status_e status = ALIPAY_PAYMENT_STATUS_UNKNOWN;
  char ext[256];
  uint32_t ext_length = sizeof(ext);
  out->sdk_rc =
      alipay_query_last_nfc_pay_result(&status, out->amount, ext, &ext_length);
  out->sdk_status = (int)status;
  out->response_sent = true;
  out->amount[sizeof(out->amount) - 1u] = '\0';
  state(out);
  if (out->state != H2_ALIPAY_PAYMENT_PAID)
    wipe(out->amount, sizeof(out->amount));
  wipe(ext, sizeof(ext));
  return leave(user, H2_PAL_OK);
}
static int stop_tap(void* user) {
  int rc = enter(user);
  if (rc != H2_PAL_OK) return rc;
  s_tap_active = false;
  s_received_ndef = s_notify_failed = false;
  s_owner = NULL;
  wipe(&s_transport, sizeof(s_transport));
  wipe(&s_tap, sizeof(s_tap));
  wipe(&s_pending, sizeof(s_pending));
  return leave(user, H2_PAL_OK);
}

static int receive_ble_frame(void* user, const uint8_t* data, size_t length,
                             uint16_t att_mtu,
                             h2_alipay_payment_result_t* out) {
  if (data == NULL || length == 0u || length > 512u || att_mtu < 23u ||
      out == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  int rc = enter(user);
  if (rc != H2_PAL_OK) return rc;
  if (!s_tap_active || !s_received_ndef || s_transport.notify == NULL ||
      s_tap.response_sent || s_notify_failed ||
      (s_pending.valid && s_pending.notify))
    return leave(user, H2_PAL_ERR_INVALID_STATE);
  wipe(&s_pending, sizeof(s_pending));
  s_tap.confirmation_required = false;
  alipay_npay_set_ble_mtu_size(att_mtu >= 103u ? 101u : 20u);
  s_tap.transport_rc = H2_PAL_OK;
  s_tap.sdk_rc = alipay_npay_ble_recv_data_handle(data, (uint32_t)length);
  *out = s_tap;
  record(user, "alipay_ble_receive.return", out->sdk_rc,
         (uint32_t)out->transport_rc);
  rc = out->transport_rc;
  if (rc == H2_PAL_OK && out->sdk_rc != 0) rc = H2_PAL_ERR_IO;
  return leave(user, rc);
}
const h2_alipay_payment_vtable_t* h2_alipay_payment_bridge_vtable(void) {
  static const h2_alipay_payment_vtable_t vtable = {
      .get_code = get_code,
      .query_code = query_code,
      .start_tap = start_tap,
      .receive_ndef = receive_ndef,
      .query_tap = query_tap,
      .receive_ble_result = receive_ble_result,
      .confirm_tap = confirm_tap,
      .stop_tap = stop_tap,
      .receive_ble_frame = receive_ble_frame};
  return &vtable;
}
