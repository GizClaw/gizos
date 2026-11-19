#include "h2_esp_nfc_fm175xx.h"
#include "h2_esp_nfc_fm175xx_internal.h"
#include "h2_sha256.h"

#include <limits.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define H2_ESP_NFC_FM175XX_NDEF_UPDATE_TIMEOUT_MS 500u

static void report_log(const h2_esp_nfc_fm175xx_t* adapter, const char* format,
                       ...) {
  const h2_pal_log_api_t* log = adapter->config.log;
  if (log == NULL || log->vtable == NULL || log->vtable->write == NULL) return;
  /* Include the complete register diagnostics in one bounded record. */
  char message[512];
  va_list args;
  va_start(args, format);
  int length = vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  if (length < 0 || (size_t)length >= sizeof(message)) return;
  (void)h2_pal_log_write(log, H2_PAL_LOG_WARN, "h2_nfc_fm175xx", message);
}

static h2_pal_result_t pal_rc_from_fm(int rc) {
  switch (rc) {
    case H2_FM175XX_OK:
      return H2_PAL_OK;
    case H2_FM175XX_ERR_INVALID_ARG:
      return H2_PAL_ERR_INVALID_ARG;
    case H2_FM175XX_ERR_UNAVAILABLE:
      return H2_PAL_ERR_UNAVAILABLE;
    case H2_FM175XX_ERR_NO_MEMORY:
      return H2_PAL_ERR_NO_MEMORY;
    case H2_FM175XX_ERR_TIMEOUT:
      return H2_PAL_ERR_TIMEOUT;
    case H2_FM175XX_ERR_IO:
    case H2_FM175XX_ERR_PROTOCOL:
    default:
      return H2_PAL_ERR_IO;
  }
}

static void clear_ndef_update(h2_esp_nfc_fm175xx_t* adapter) {
  adapter->prepared_ndef_id = 0u;
  adapter->read_selection_valid = 0;
  adapter->ndef_update.size = 0u;
  memset(adapter->ndef_update.raw, 0, sizeof(adapter->ndef_update.raw));
}

static bool ndef_update_active(h2_esp_nfc_fm175xx_t* adapter) {
  if (adapter->prepared_ndef_id == 0u) return false;
  uint64_t now_ms;
  if (h2_pal_time_get_monotonic_ms(adapter->config.time, &now_ms) ==
          H2_PAL_OK &&
      now_ms - adapter->ndef_update_started_ms <
          H2_ESP_NFC_FM175XX_NDEF_UPDATE_TIMEOUT_MS)
    return true;
  clear_ndef_update(adapter);
  return false;
}

static h2_pal_result_t ensure_open(h2_esp_nfc_fm175xx_t* adapter) {
  if (adapter == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (adapter->opened != 0) {
    return H2_PAL_OK;
  }
  adapter->read_selection_valid = 0;
  h2_fm175xx_transport_t transport;
  memset(&transport, 0, sizeof(transport));
  if (adapter->transport_kind == H2_ESP_NFC_FM175XX_TRANSPORT_SPI) {
    if (h2_esp_nfc_fm175xx_spi_open(adapter, &transport) != H2_FM175XX_OK) {
      return H2_PAL_ERR_IO;
    }
  } else {
    if (h2_esp_nfc_fm175xx_i2c_open(adapter, &transport) != H2_FM175XX_OK) {
      return H2_PAL_ERR_IO;
    }
  }
  int rc = h2_fm175xx_init(&adapter->reader, &transport);
  if (rc == H2_FM175XX_OK) {
    rc = h2_fm175xx_open_type_a(&adapter->reader);
  }
  if (rc != H2_FM175XX_OK) {
    h2_esp_nfc_fm175xx_deinit(adapter);
    return pal_rc_from_fm(rc);
  }

  adapter->opened = 1;
  return H2_PAL_OK;
}

/* Inspect control/status only; never consume FIFO or print a card UID. */
static void report_activation(h2_esp_nfc_fm175xx_t* adapter, int result,
                              const h2_fm175xx_type_a_card_t* card) {
  const uint8_t addresses[] = {0x37u, 0x11u, 0x14u, 0x12u, 0x13u, 0x0du,
                               0x04u, 0x06u, 0x0au, 0x26u, 0x18u, 0x2au};
  uint8_t values[sizeof(addresses)] = {0};
  int reads = H2_FM175XX_OK;
  const h2_fm175xx_transport_t* transport = &adapter->reader.transport;
  for (size_t i = 0u; i < sizeof(addresses); ++i) {
    const int rc =
        transport->read_reg(transport->user, addresses[i], &values[i]);
    if (reads == H2_FM175XX_OK && rc != H2_FM175XX_OK) reads = rc;
  }
  report_log(adapter,
             "H2_NFC_DIAG fm=%d atqa_seen=%u uid_bytes=%u version=0x%02x "
             "mode=0x%02x txctrl=0x%02x txmode=0x%02x rxmode=0x%02x "
             "framing=0x%02x irq=0x%02x error=0x%02x fifo=%u gain=0x%02x "
             "threshold=0x%02x timer=0x%02x reads=%d",
             result, (unsigned)((card->atqa[0] | card->atqa[1]) != 0u),
             (unsigned)card->uid_len, (unsigned)values[0], (unsigned)values[1],
             (unsigned)values[2], (unsigned)values[3], (unsigned)values[4],
             (unsigned)values[5], (unsigned)values[6], (unsigned)values[7],
             (unsigned)values[8], (unsigned)values[9], (unsigned)values[10],
             (unsigned)values[11], reads);
}

static h2_pal_result_t scan_nfc_reader(void* user, h2_pal_periph_id_t id,
                                       h2_pal_nfc_scan_t* out_scan) {
  h2_esp_nfc_fm175xx_t* adapter = (h2_esp_nfc_fm175xx_t*)user;
  if (adapter == NULL || out_scan == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (id != adapter->config.id) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  if (ndef_update_active(adapter)) return H2_PAL_ERR_BUSY;

  memset(out_scan, 0, sizeof(*out_scan));
  out_scan->id = id;
  out_scan->stage = H2_PAL_NFC_STAGE_ERROR;
  out_scan->result = H2_PAL_OK;
  adapter->read_selection_valid = 0;

  h2_pal_result_t prc = ensure_open(adapter);
  if (prc != H2_PAL_OK) {
    out_scan->result = prc;
    return prc;
  }

  h2_fm175xx_type_a_card_t card;
  const int rc = h2_fm175xx_type_a_activate(&adapter->reader, &card);
  ++adapter->scan_count;
  if (adapter->scan_count == 1u || adapter->last_scan_error != rc ||
      adapter->scan_count % 64u == 0u) {
    report_activation(adapter, rc, &card);
  }
  adapter->last_scan_error = rc;
  if (rc == H2_FM175XX_ERR_TIMEOUT) {
    out_scan->stage = H2_PAL_NFC_STAGE_ABSENT;
    out_scan->result = H2_PAL_OK;
    out_scan->tag_type = H2_PAL_NFC_TAG_TYPE_UNKNOWN;
    return H2_PAL_OK;
  }
  if (rc != H2_FM175XX_OK) {
    out_scan->stage = H2_PAL_NFC_STAGE_ERROR;
    out_scan->result = pal_rc_from_fm(rc);
    out_scan->tag_type = H2_PAL_NFC_TAG_TYPE_UNKNOWN;
    return out_scan->result;
  }

  out_scan->stage = H2_PAL_NFC_STAGE_DISCOVERED;
  out_scan->result = H2_PAL_OK;
  out_scan->tag_type = H2_PAL_NFC_TAG_TYPE_ISO14443A;
  out_scan->uid_len = card.uid_len > H2_PAL_NFC_UID_MAX_LEN
                          ? H2_PAL_NFC_UID_MAX_LEN
                          : card.uid_len;
  memcpy(out_scan->uid, card.uid, out_scan->uid_len);
  return H2_PAL_OK;
}

static int uid_matches(const h2_fm175xx_type_a_card_t* card,
                       const uint8_t* expected_uid, uint8_t expected_uid_len) {
  if (expected_uid_len == 0u) {
    return 1;
  }
  return card->uid_len == expected_uid_len &&
         memcmp(card->uid, expected_uid, expected_uid_len) == 0;
}

static void report_read(const h2_esp_nfc_fm175xx_t* adapter, const char* phase,
                        h2_pal_result_t result, int fm,
                        h2_pal_nfc_data_type_t type, uint8_t expected_uid_len,
                        const h2_fm175xx_type_a_card_t* card, size_t length) {
  const h2_fm175xx_type_a_card_t empty = {0};
  if (card == NULL) card = &empty;
  report_log(adapter,
             "H2_NFC_READ phase=%s fm=%d pal_rc=%d requested=%u "
             "expected_uid_bytes=%u uid_bytes=%u atqa0=0x%02x atqa1=0x%02x "
             "sak_levels=%u sak0=0x%02x sak1=0x%02x sak2=0x%02x bytes=%zu",
             phase, fm, result, (unsigned)type, (unsigned)expected_uid_len,
             (unsigned)card->uid_len, (unsigned)card->atqa[0],
             (unsigned)card->atqa[1], (unsigned)card->sak_len,
             (unsigned)card->sak[0], (unsigned)card->sak[1],
             (unsigned)card->sak[2], length);
}

static h2_pal_result_t read_nfc(void* user, h2_pal_periph_id_t id,
                                const uint8_t* expected_uid,
                                uint8_t expected_uid_len,
                                h2_pal_nfc_data_type_t requested_type,
                                const h2_pal_mem_api_t* allocator,
                                h2_pal_nfc_data_read_t* out_data,
                                bool prepare) {
  h2_esp_nfc_fm175xx_t* adapter = (h2_esp_nfc_fm175xx_t*)user;
  if (out_data == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (adapter == NULL || allocator == NULL ||
      (expected_uid == NULL && expected_uid_len != 0u) ||
      expected_uid_len > H2_PAL_NFC_UID_MAX_LEN) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (id != adapter->config.id) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  if (ndef_update_active(adapter)) return H2_PAL_ERR_BUSY;
  memset(out_data, 0, sizeof(*out_data));
  if (prepare && adapter->config.time == NULL) return H2_PAL_ERR_UNSUPPORTED;
  if (requested_type != H2_PAL_NFC_DATA_NTAG_PAGES &&
      requested_type != H2_PAL_NFC_DATA_NDEF) {
    return H2_PAL_ERR_UNSUPPORTED;
  }

  report_read(adapter, "begin", H2_PAL_OK, INT_MIN, requested_type,
              expected_uid_len, NULL, 0u);
  adapter->read_selection_valid = 0;
  h2_pal_result_t prc = ensure_open(adapter);
  if (prc != H2_PAL_OK) {
    report_read(adapter, "open", prc, INT_MIN, requested_type, expected_uid_len,
                NULL, 0u);
    return prc;
  }

  h2_fm175xx_type_a_card_t card = {0};
  h2_fm175xx_activation_diagnostics_t selection;
  int rc = h2_fm175xx_type_a_activate_diagnostic(&adapter->reader, &card,
                                                 &selection);
  if (rc != H2_FM175XX_OK)
    report_log(
        adapter,
        "H2_NFC_READ_SELECT phase=%u command=0x%02x cascade=%u bits=%zu "
        "fm=%d status_rc=%d irq=0x%02x error=0x%02x fifo=%u control=0x%02x",
        (unsigned)selection.phase, (unsigned)selection.command,
        (unsigned)selection.cascade_level, selection.reply_bits, rc,
        selection.status_result, (unsigned)selection.last_irq,
        (unsigned)selection.last_error, (unsigned)selection.last_fifo,
        (unsigned)selection.last_control);
  prc =
      rc == H2_FM175XX_ERR_TIMEOUT ? H2_PAL_ERR_NOT_FOUND : pal_rc_from_fm(rc);
  report_read(adapter, "activate", prc, rc, requested_type, expected_uid_len,
              &card, 0u);
  if (rc == H2_FM175XX_ERR_TIMEOUT) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  if (rc != H2_FM175XX_OK) {
    return pal_rc_from_fm(rc);
  }
  if (!uid_matches(&card, expected_uid, expected_uid_len)) {
    report_read(adapter, "uid", H2_PAL_ERR_INVALID_STATE, H2_FM175XX_OK,
                requested_type, expected_uid_len, &card, 0u);
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (requested_type == H2_PAL_NFC_DATA_NDEF &&
      (card.sak_len == 0u || card.sak[card.sak_len - 1u] != 0u)) {
    report_read(adapter, "tag_type", H2_PAL_ERR_UNSUPPORTED, H2_FM175XX_OK,
                requested_type, expected_uid_len, &card, 0u);
    return H2_PAL_ERR_UNSUPPORTED;
  }

  size_t capacity = adapter->config.read_capacity;
  if (capacity == 0u) {
    capacity = H2_ESP_NFC_FM175XX_DEFAULT_READ_CAPACITY;
  }
  uint8_t* bytes = (uint8_t*)h2_pal_mem_alloc(allocator, capacity);
  if (bytes == NULL) {
    report_read(adapter, "allocate", H2_PAL_ERR_NO_MEMORY, INT_MIN,
                requested_type, expected_uid_len, &card, 0u);
    return H2_PAL_ERR_NO_MEMORY;
  }

  size_t len = 0u;
  h2_fm175xx_read_diagnostics_t diagnostics = {0};
  if (prepare) {
    h2_fm175xx_write_diagnostics_t preparation;
    rc = h2_fm175xx_ntag_prepare_ndef_update(
        &adapter->reader, &adapter->ndef_update, &preparation);
    diagnostics = preparation.preread;
    if (rc == H2_FM175XX_OK) {
      len = adapter->ndef_update.value_length;
      if (len == 0u)
        rc = H2_FM175XX_ERR_PROTOCOL;
      else if (len > capacity)
        rc = H2_FM175XX_ERR_NO_MEMORY;
      else
        memcpy(bytes,
               adapter->ndef_update.raw + adapter->ndef_update.value_offset,
               len);
    }
  } else if (requested_type == H2_PAL_NFC_DATA_NDEF) {
    rc = h2_fm175xx_ntag_read_ndef_diagnostic(&adapter->reader, bytes, capacity,
                                              &len, &diagnostics);
  } else {
    rc = h2_fm175xx_ntag_read_all(&adapter->reader, bytes, capacity, &len);
  }
  if (requested_type == H2_PAL_NFC_DATA_NDEF)
    report_log(adapter,
               "H2_NFC_READ_FRAMES count=%zu crc_frames=%zu last_page=%u "
               "last_bits=%zu last_fm=%d capacity=%zu required=%zu "
               "status_rc=%d irq=0x%02x error=0x%02x fifo=%u control=0x%02x "
               "rx_status=%d rx_irq=0x%02x rx_error=0x%02x rx_fifo=%u "
               "rx_control=0x%02x",
               diagnostics.read_count, diagnostics.retained_crc_frames,
               (unsigned)diagnostics.last_page, diagnostics.last_reply_bits,
               diagnostics.last_result, capacity, diagnostics.required_capacity,
               diagnostics.status_result, (unsigned)diagnostics.last_irq,
               (unsigned)diagnostics.last_error,
               (unsigned)diagnostics.last_fifo,
               (unsigned)diagnostics.last_control, diagnostics.rx_status_result,
               (unsigned)diagnostics.rx_irq, (unsigned)diagnostics.rx_error,
               (unsigned)diagnostics.rx_fifo, (unsigned)diagnostics.rx_control);
  prc = pal_rc_from_fm(rc);
  if (prc == H2_PAL_OK && prepare)
    prc = h2_pal_time_get_monotonic_ms(adapter->config.time,
                                       &adapter->ndef_update_started_ms);
  report_read(adapter, "content", prc, rc, requested_type, expected_uid_len,
              &card, len);
  if (prc != H2_PAL_OK) {
    if (prepare) clear_ndef_update(adapter);
    h2_pal_mem_free(allocator, bytes);
    return prc;
  }

  out_data->id = id;
  out_data->tag_type = H2_PAL_NFC_TAG_TYPE_NTAG;
  out_data->uid_len = card.uid_len > H2_PAL_NFC_UID_MAX_LEN
                          ? H2_PAL_NFC_UID_MAX_LEN
                          : card.uid_len;
  memcpy(out_data->uid, card.uid, out_data->uid_len);
  out_data->type = requested_type;
  out_data->bytes = bytes;
  out_data->len = len;
  adapter->read_selection = card;
  adapter->read_selection_valid = 1;
  if (prepare) adapter->prepared_ndef_id = (uintptr_t)out_data;
  return H2_PAL_OK;
}

static h2_pal_result_t read_nfc_data(void* user, h2_pal_periph_id_t id,
                                     const uint8_t* expected_uid,
                                     uint8_t expected_uid_len,
                                     h2_pal_nfc_data_type_t type,
                                     const h2_pal_mem_api_t* allocator,
                                     h2_pal_nfc_data_read_t* out_data) {
  return read_nfc(user, id, expected_uid, expected_uid_len, type, allocator,
                  out_data, false);
}

static h2_pal_result_t prepare_ndef_write(void* user, h2_pal_periph_id_t id,
                                          const uint8_t* expected_uid,
                                          uint8_t expected_uid_len,
                                          const h2_pal_mem_api_t* allocator,
                                          h2_pal_nfc_data_read_t* out_data) {
  return read_nfc(user, id, expected_uid, expected_uid_len,
                  H2_PAL_NFC_DATA_NDEF, allocator, out_data, true);
}

static void report_write(const h2_esp_nfc_fm175xx_t* adapter, const char* phase,
                         int result, int fm, size_t length) {
  report_log(adapter,
             "H2_NFC_WRITE phase=%s fm=%d pal_rc=%d bytes=%zu submitted=%u",
             phase, fm, result, length,
             (unsigned)(strcmp(phase, "content") == 0 && result == H2_PAL_OK));
}

static const char* activation_phase_name(h2_fm175xx_activation_phase_t phase) {
  switch (phase) {
    case H2_FM175XX_ACTIVATION_FIELD:
      return "field";
    case H2_FM175XX_ACTIVATION_HALT:
      return "halt";
    case H2_FM175XX_ACTIVATION_REQUEST:
      return "request";
    case H2_FM175XX_ACTIVATION_ANTICOLLISION:
      return "anticollision";
    case H2_FM175XX_ACTIVATION_SELECT:
      return "select";
    case H2_FM175XX_ACTIVATION_COMPLETE:
      return "complete";
    default:
      return "unknown";
  }
}

static int select_write_tag(h2_esp_nfc_fm175xx_t* adapter,
                            h2_fm175xx_type_a_card_t* card, size_t length) {
  for (unsigned attempt = 1u; attempt <= 3u; ++attempt) {
    h2_fm175xx_activation_diagnostics_t diagnostics;
    int rc = h2_fm175xx_type_a_activate_diagnostic(&adapter->reader, card,
                                                   &diagnostics);
    report_log(
        adapter,
        "H2_NFC_WRITE_SELECT attempt=%u limit=3 phase=%s command=0x%02x "
        "cascade=%u bits=%zu fm=%d status_rc=%d irq=0x%02x "
        "error=0x%02x fifo=%u control=0x%02x",
        attempt, activation_phase_name(diagnostics.phase),
        (unsigned)diagnostics.command, (unsigned)diagnostics.cascade_level,
        diagnostics.reply_bits, rc, diagnostics.status_result,
        (unsigned)diagnostics.last_irq, (unsigned)diagnostics.last_error,
        (unsigned)diagnostics.last_fifo, (unsigned)diagnostics.last_control);
    report_write(adapter, "activate",
                 rc == H2_FM175XX_ERR_TIMEOUT ? H2_PAL_ERR_NOT_FOUND
                                              : pal_rc_from_fm(rc),
                 rc, length);
    if (rc == H2_FM175XX_OK || attempt == 3u ||
        (rc != H2_FM175XX_ERR_TIMEOUT && rc != H2_FM175XX_ERR_IO &&
         rc != H2_FM175XX_ERR_PROTOCOL))
      return rc;
    const h2_fm175xx_transport_t* transport = &adapter->reader.transport;
    if (transport->sleep_ms != NULL) transport->sleep_ms(transport->user, 20u);
  }
  return H2_FM175XX_ERR_TIMEOUT;
}

static const char* write_phase_name(h2_fm175xx_write_phase_t phase) {
  switch (phase) {
    case H2_FM175XX_WRITE_ARGUMENT:
      return "argument";
    case H2_FM175XX_WRITE_READ_TAG:
      return "read_tag";
    case H2_FM175XX_WRITE_CC:
      return "cc";
    case H2_FM175XX_WRITE_LOCKS:
      return "locks";
    case H2_FM175XX_WRITE_LAYOUT:
      return "layout";
    case H2_FM175XX_WRITE_TRAILING_TLV:
      return "trailing_tlv";
    case H2_FM175XX_WRITE_CAPACITY:
      return "capacity";
    case H2_FM175XX_WRITE_INVALIDATE:
      return "invalidate";
    case H2_FM175XX_WRITE_PAYLOAD:
      return "payload";
    case H2_FM175XX_WRITE_PUBLISH:
      return "publish";
    case H2_FM175XX_WRITE_COMPLETE:
      return "complete";
    default:
      return "unknown";
  }
}

static void report_ndef_fingerprint(const h2_esp_nfc_fm175xx_t* adapter,
                                    const char* phase, uint8_t valid,
                                    size_t length, const uint8_t digest[32]) {
  if (!valid) return;
  char hex[65];
  h2_sha256_hex(digest, hex);
  report_log(adapter, "H2_NFC_WRITE_FINGERPRINT phase=%s bytes=%zu sha256=%s",
             phase, length, hex);
}

static h2_pal_result_t write_nfc(void* user, h2_pal_periph_id_t id,
                                 const uint8_t* expected_uid,
                                 uint8_t expected_uid_len,
                                 h2_pal_nfc_data_type_t type,
                                 const uint8_t* bytes, size_t length,
                                 bool prepared) {
  h2_esp_nfc_fm175xx_t* adapter = user;
  if (adapter == NULL || expected_uid == NULL || expected_uid_len == 0u ||
      expected_uid_len > H2_PAL_NFC_UID_MAX_LEN || bytes == NULL ||
      length == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  if (id != adapter->config.id) return H2_PAL_ERR_NOT_FOUND;
  if (type != H2_PAL_NFC_DATA_NDEF) return H2_PAL_ERR_UNSUPPORTED;
  if (!prepared && ndef_update_active(adapter)) return H2_PAL_ERR_BUSY;
  report_write(adapter, "begin", H2_PAL_OK, INT_MIN, length);
  int rc = ensure_open(adapter);
  if (rc != H2_PAL_OK) {
    report_write(adapter, "open", rc, INT_MIN, length);
    return rc;
  }
  h2_fm175xx_type_a_card_t card;
  int reuse =
      adapter->read_selection_valid &&
      uid_matches(&adapter->read_selection, expected_uid, expected_uid_len);
  if (reuse) card = adapter->read_selection;
  adapter->read_selection_valid = 0;
  if (reuse) {
    report_log(adapter, "H2_NFC_WRITE_SELECT mode=read-session fm=0");
    report_write(adapter, "activate", H2_PAL_OK, H2_FM175XX_OK, length);
  } else if (!prepared) {
    rc = select_write_tag(adapter, &card, length);
  } else {
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (rc == H2_FM175XX_ERR_TIMEOUT) return H2_PAL_ERR_NOT_FOUND;
  if (rc != H2_FM175XX_OK) return pal_rc_from_fm(rc);
  if (!uid_matches(&card, expected_uid, expected_uid_len)) {
    report_write(adapter, "uid", H2_PAL_ERR_INVALID_STATE, rc, length);
    return H2_PAL_ERR_INVALID_STATE;
  }
  /* The Type 2 implementation must not send page commands to ISO-DEP cards. */
  if (card.sak_len == 0u || card.sak[card.sak_len - 1u] != 0u) {
    report_write(adapter, "tag_type", H2_PAL_ERR_UNSUPPORTED, rc, length);
    return H2_PAL_ERR_UNSUPPORTED;
  }
  h2_fm175xx_write_diagnostics_t diagnostics;
  rc = prepared ? h2_fm175xx_ntag_commit_ndef_update(
                      &adapter->reader, &adapter->ndef_update, bytes, length,
                      &diagnostics)
                : h2_fm175xx_ntag_write_ndef_diagnostic(&adapter->reader, bytes,
                                                        length, &diagnostics);
  report_log(adapter,
             "H2_NFC_WRITE_DETAIL phase=%s fm=%d capacity=%zu metadata=%u "
             "cc=%02x%02x%02x%02x locks=%02x%02x first_tlv=0x%02x "
             "write_calls=%zu last_page=%u ndef_offset=%zu control_tlvs=%zu "
             "control=%02x%02x%02x%02x%02x",
             write_phase_name(diagnostics.phase), rc, diagnostics.tag_capacity,
             (unsigned)diagnostics.metadata_valid, (unsigned)diagnostics.cc[0],
             (unsigned)diagnostics.cc[1], (unsigned)diagnostics.cc[2],
             (unsigned)diagnostics.cc[3], (unsigned)diagnostics.static_locks[0],
             (unsigned)diagnostics.static_locks[1],
             (unsigned)diagnostics.first_tlv, diagnostics.write_calls,
             (unsigned)diagnostics.last_page, diagnostics.ndef_offset,
             diagnostics.control_tlvs, (unsigned)diagnostics.control_tlv[0],
             (unsigned)diagnostics.control_tlv[1],
             (unsigned)diagnostics.control_tlv[2],
             (unsigned)diagnostics.control_tlv[3],
             (unsigned)diagnostics.control_tlv[4]);
  if (diagnostics.preread.read_count != 0u) {
    const h2_fm175xx_read_diagnostics_t* read = &diagnostics.preread;
    report_log(
        adapter,
        "H2_NFC_WRITE_PREREAD count=%zu page=%u bits=%zu fm=%d "
        "transfer_rc=%d required=%zu header_valid=%u "
        "cc=%02x%02x%02x%02x locks=%02x%02x "
        "short_valid=%u low4=0x%01x status_rc=%d "
        "irq=0x%02x error=0x%02x fifo=%u control=0x%02x "
        "rx_status=%d rx_irq=0x%02x rx_error=0x%02x "
        "rx_fifo=%u rx_control=0x%02x",
        read->read_count, (unsigned)read->last_page, read->last_reply_bits,
        read->last_result, read->transfer_result, read->required_capacity,
        (unsigned)diagnostics.preread_header_valid,
        (unsigned)diagnostics.preread_cc[0],
        (unsigned)diagnostics.preread_cc[1],
        (unsigned)diagnostics.preread_cc[2],
        (unsigned)diagnostics.preread_cc[3],
        (unsigned)diagnostics.preread_static_locks[0],
        (unsigned)diagnostics.preread_static_locks[1],
        (unsigned)read->short_reply_valid, (unsigned)read->short_reply_low4,
        read->status_result, (unsigned)read->last_irq,
        (unsigned)read->last_error, (unsigned)read->last_fifo,
        (unsigned)read->last_control, read->rx_status_result,
        (unsigned)read->rx_irq, (unsigned)read->rx_error,
        (unsigned)read->rx_fifo, (unsigned)read->rx_control);
  }
  if (diagnostics.write_calls != 0u) {
    report_log(
        adapter,
        "H2_NFC_WRITE_REPLY page=%u bits=%zu low4=0x%01x "
        "command_rc=%d restore_rc=%d status_rc=%d irq=0x%02x "
        "error=0x%02x fifo=%u control=0x%02x crc_valid=%u",
        (unsigned)diagnostics.last_page, diagnostics.last_reply_bits,
        (unsigned)diagnostics.last_reply_low4, diagnostics.last_command_result,
        diagnostics.last_restore_result, diagnostics.status_result,
        (unsigned)diagnostics.last_irq, (unsigned)diagnostics.last_error,
        (unsigned)diagnostics.last_fifo, (unsigned)diagnostics.last_control,
        (unsigned)diagnostics.last_reply_crc_valid);
    report_log(
        adapter,
        "H2_NFC_WRITE_FRAME bytes=%02x%02x%02x rx_status=%d "
        "irq=0x%02x error=0x%02x fifo=%u control=0x%02x "
        "config_status=%d tx=0x%02x rx=0x%02x framing=0x%02x",
        (unsigned)diagnostics.last_reply[0],
        (unsigned)diagnostics.last_reply[1],
        (unsigned)diagnostics.last_reply[2], diagnostics.rx_status_result,
        (unsigned)diagnostics.rx_irq, (unsigned)diagnostics.rx_error,
        (unsigned)diagnostics.rx_fifo, (unsigned)diagnostics.rx_control,
        diagnostics.configuration_result, (unsigned)diagnostics.last_tx_mode,
        (unsigned)diagnostics.last_rx_mode, (unsigned)diagnostics.last_framing);
  }
  report_ndef_fingerprint(adapter, "before", diagnostics.before_ndef_valid,
                          diagnostics.before_ndef_length,
                          diagnostics.before_ndef_sha256);
  report_ndef_fingerprint(adapter, "expected", diagnostics.expected_ndef_valid,
                          length, diagnostics.expected_ndef_sha256);
  report_write(adapter, "content", pal_rc_from_fm(rc), rc, length);
  return pal_rc_from_fm(rc);
}

static h2_pal_result_t write_nfc_data(void* user, h2_pal_periph_id_t id,
                                      const uint8_t* expected_uid,
                                      uint8_t expected_uid_len,
                                      h2_pal_nfc_data_type_t type,
                                      const uint8_t* bytes, size_t length) {
  return write_nfc(user, id, expected_uid, expected_uid_len, type, bytes,
                   length, false);
}

static h2_pal_result_t finish_ndef_write(void* user, h2_pal_periph_id_t id,
                                         const h2_pal_nfc_data_read_t* prepared,
                                         const uint8_t* bytes, size_t length) {
  h2_esp_nfc_fm175xx_t* adapter = user;
  if (adapter == NULL || prepared == NULL || (bytes == NULL) != (length == 0u))
    return H2_PAL_ERR_INVALID_ARG;
  if (id != adapter->config.id) return H2_PAL_ERR_NOT_FOUND;
  if (!ndef_update_active(adapter) ||
      adapter->prepared_ndef_id != (uintptr_t)prepared)
    return H2_PAL_ERR_INVALID_STATE;
  h2_pal_result_t rc = H2_PAL_OK;
  if (bytes != NULL) {
    const h2_fm175xx_type_a_card_t* card = &adapter->read_selection;
    rc = write_nfc(adapter, id, card->uid, card->uid_len, H2_PAL_NFC_DATA_NDEF,
                   bytes, length, true);
  }
  clear_ndef_update(adapter);
  return rc;
}

const h2_pal_nfc_vtable_t h2_esp_nfc_fm175xx_nfc_vtable = {
    .scan_nfc_reader = scan_nfc_reader,
    .read_nfc_data = read_nfc_data,
    .write_nfc_data = write_nfc_data,
    .prepare_ndef_write = prepare_ndef_write,
    .finish_ndef_write = finish_ndef_write,
};

h2_pal_result_t h2_esp_nfc_fm175xx_init(
    h2_esp_nfc_fm175xx_t* adapter, const h2_esp_nfc_fm175xx_config_t* config) {
  if (adapter == NULL || config == NULL || config->id == 0u ||
      config->bus == NULL || config->i2c_address == 0u ||
      config->i2c_speed_hz == 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(adapter, 0, sizeof(*adapter));
  adapter->config = *config;
  adapter->transport_kind = H2_ESP_NFC_FM175XX_TRANSPORT_I2C;
  adapter->api.user = adapter;
  adapter->api.vtable = &h2_esp_nfc_fm175xx_nfc_vtable;
  return H2_PAL_OK;
}

void h2_esp_nfc_fm175xx_deinit(h2_esp_nfc_fm175xx_t* adapter) {
  if (adapter == NULL) {
    return;
  }
  if (adapter->transport_kind == H2_ESP_NFC_FM175XX_TRANSPORT_SPI) {
    h2_esp_nfc_fm175xx_spi_close(adapter);
  } else {
    h2_esp_nfc_fm175xx_i2c_close(adapter);
  }
  memset(&adapter->reader, 0, sizeof(adapter->reader));
  adapter->opened = 0;
  memset(&adapter->read_selection, 0, sizeof(adapter->read_selection));
  clear_ndef_update(adapter);
}

const h2_pal_nfc_api_t* h2_esp_nfc_fm175xx_api(h2_esp_nfc_fm175xx_t* adapter) {
  return adapter == NULL ? NULL : &adapter->api;
}
