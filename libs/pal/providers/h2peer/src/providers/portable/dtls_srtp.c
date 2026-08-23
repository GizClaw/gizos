#include "dtls_srtp.h"

#include <limits.h>
#include <string.h>

#include "config.h"
#include "utils.h"

static void dtls_srtp_close_srtp(DtlsSrtp* dtls_srtp) {
  h2_libsrtp_session_destroy(&dtls_srtp->srtp_in);
  h2_libsrtp_session_destroy(&dtls_srtp->srtp_out);
}

static h2_pal_result_t dtls_srtp_send_datagram(
    void* user, const uint8_t* datagram, size_t datagram_len) {
  DtlsSrtp* dtls_srtp = (DtlsSrtp*)user;
  if (dtls_srtp == NULL || dtls_srtp->packet_send == NULL) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  int result = dtls_srtp->packet_send(dtls_srtp, datagram, datagram_len);
  if (result >= 0) {
    return H2_PAL_OK;
  }
  if (result == H2_PAL_ERR_CLOSED) {
    return H2_PAL_ERR_CLOSED;
  }
  return result == H2_PAL_ERR_WOULD_BLOCK ||
                 result == H2_PAL_ERR_TIMEOUT
             ? H2_PAL_ERR_WOULD_BLOCK
             : H2_PAL_ERR_IO;
}

static h2_pal_result_t dtls_srtp_plaintext(
    void* user, const uint8_t* plaintext, size_t plaintext_len) {
  DtlsSrtp* dtls_srtp = (DtlsSrtp*)user;
  if (dtls_srtp == NULL || dtls_srtp->plaintext_out == NULL) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (plaintext_len > dtls_srtp->plaintext_capacity ||
      dtls_srtp->plaintext_len != 0u) {
    dtls_srtp->plaintext_result = H2_PAL_ERR_TRUNCATED;
    return dtls_srtp->plaintext_result;
  }
  memcpy(dtls_srtp->plaintext_out, plaintext, plaintext_len);
  dtls_srtp->plaintext_len = plaintext_len;
  return H2_PAL_OK;
}

static void dtls_srtp_format_fingerprint(
    const uint8_t fingerprint[H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE],
    char out[DTLS_SRTP_FINGERPRINT_LENGTH]) {
  static const char digits[] = "0123456789ABCDEF";
  size_t pos = 0u;
  for (size_t i = 0u; i < H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE; ++i) {
    if (i != 0u) {
      out[pos++] = ':';
    }
    out[pos++] = digits[fingerprint[i] >> 4u];
    out[pos++] = digits[fingerprint[i] & 0x0fu];
  }
  out[pos] = '\0';
}

static int dtls_srtp_hex_digit(char value) {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  return -1;
}

static int dtls_srtp_parse_fingerprint(
    const char* text,
    uint8_t out[H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE]) {
  if (text == NULL) {
    return -1;
  }
  for (size_t i = 0u; i < H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE; ++i) {
    int high = dtls_srtp_hex_digit(*text++);
    int low = dtls_srtp_hex_digit(*text++);
    if (high < 0 || low < 0) {
      return -1;
    }
    out[i] = (uint8_t)((high << 4) | low);
    if (i + 1u != H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE && *text++ != ':') {
      return -1;
    }
  }
  return *text == '\0' ? 0 : -1;
}

static int dtls_srtp_create_session(
    h2_libsrtp_direction_t direction,
    const uint8_t* key,
    const uint8_t* salt,
    h2_libsrtp_session_t** out_session) {
  const h2_libsrtp_session_config_t config = {
      .profile = H2_LIBSRTP_PROFILE_AES128_CM_SHA1_80,
      .direction = direction,
      .ssrc_policy = H2_LIBSRTP_SSRC_ANY,
      .master_key = key,
      .master_key_len = H2_LIBSRTP_MASTER_KEY_SIZE,
      .master_salt = salt,
      .master_salt_len = H2_LIBSRTP_AES_CM_SALT_SIZE,
  };
  return h2_libsrtp_session_create(&config, out_session) == H2_PAL_OK ? 0 : -1;
}

static int dtls_srtp_configure_srtp(DtlsSrtp* dtls_srtp) {
  uint8_t key_material[H2_PAL_DTLS_SRTP_KEYING_MATERIAL_SIZE];
  h2_pal_dtls_srtp_profile_t profile;
  if (h2_pal_dtls_session_get_srtp_profile(
          dtls_srtp->dtls, dtls_srtp->session, &profile) != H2_PAL_OK ||
      profile != H2_PAL_DTLS_SRTP_PROFILE_AES128_CM_SHA1_80 ||
      h2_pal_dtls_session_export_srtp_keying_material(
          dtls_srtp->dtls, dtls_srtp->session,
          key_material, sizeof(key_material)) != H2_PAL_OK) {
    return -1;
  }

  const uint8_t* client_key = key_material;
  const uint8_t* server_key = client_key + H2_LIBSRTP_MASTER_KEY_SIZE;
  const uint8_t* client_salt = server_key + H2_LIBSRTP_MASTER_KEY_SIZE;
  const uint8_t* server_salt = client_salt + H2_LIBSRTP_AES_CM_SALT_SIZE;
  const uint8_t* local_key = client_key;
  const uint8_t* local_salt = client_salt;
  const uint8_t* remote_key = server_key;
  const uint8_t* remote_salt = server_salt;
  if (dtls_srtp->role == DTLS_SRTP_ROLE_SERVER) {
    local_key = server_key;
    local_salt = server_salt;
    remote_key = client_key;
    remote_salt = client_salt;
  }

  int result = dtls_srtp_create_session(
      H2_LIBSRTP_DIRECTION_INBOUND, remote_key, remote_salt,
      &dtls_srtp->srtp_in);
  if (result == 0) {
    result = dtls_srtp_create_session(
        H2_LIBSRTP_DIRECTION_OUTBOUND, local_key, local_salt,
        &dtls_srtp->srtp_out);
  }
  memset(key_material, 0, sizeof(key_material));
  if (result != 0) {
    dtls_srtp_close_srtp(dtls_srtp);
    return -1;
  }
  dtls_srtp->state = DTLS_SRTP_STATE_CONNECTED;
  return 0;
}

int dtls_srtp_init(DtlsSrtp *dtls_srtp, DtlsSrtpRole role, void *user_data,
                   const h2_pal_log_api_t *log, const h2_pal_dtls_api_t *dtls,
                   const h2_pal_time_api_t *time) {
  if (dtls_srtp == NULL || dtls == NULL || time == NULL) {
    return -1;
  }
  dtls_srtp->log = log;
  dtls_srtp->role = role;
  dtls_srtp->dtls = dtls;
  dtls_srtp->time = time;
  dtls_srtp->state = DTLS_SRTP_STATE_INIT;
  dtls_srtp->output_pending = 0;
  dtls_srtp->user_data = user_data;
  const h2_pal_dtls_session_config_t config = {
      .role = role == DTLS_SRTP_ROLE_SERVER ? H2_PAL_DTLS_ROLE_SERVER
                                           : H2_PAL_DTLS_ROLE_CLIENT,
      .max_datagram_size = CONFIG_MTU,
      .max_plaintext_size = CONFIG_MTU,
      .max_pending_output_bytes = CONFIG_MTU * 4u,
      .send = dtls_srtp_send_datagram,
      .plaintext = dtls_srtp_plaintext,
      .io_user = dtls_srtp,
  };
  if (h2_pal_dtls_session_create(dtls, &config, &dtls_srtp->session) !=
      H2_PAL_OK) {
    return -1;
  }
  uint8_t local[H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE];
  if (h2_pal_dtls_session_get_local_fingerprint(
          dtls, dtls_srtp->session, local) != H2_PAL_OK) {
    h2_pal_dtls_session_destroy(dtls, &dtls_srtp->session);
    return -1;
  }
  dtls_srtp_format_fingerprint(local, dtls_srtp->local_fingerprint);
  if (dtls_srtp->remote_fingerprint_set &&
      h2_pal_dtls_session_set_remote_fingerprint(
          dtls, dtls_srtp->session, dtls_srtp->remote_fingerprint) !=
          H2_PAL_OK) {
    h2_pal_dtls_session_destroy(dtls, &dtls_srtp->session);
    return -1;
  }
  return 0;
}

void dtls_srtp_deinit(DtlsSrtp* dtls_srtp) {
  if (dtls_srtp == NULL) {
    return;
  }
  dtls_srtp_close_srtp(dtls_srtp);
  if (dtls_srtp->dtls != NULL) {
    (void)h2_pal_dtls_session_close(dtls_srtp->dtls, dtls_srtp->session);
    h2_pal_dtls_session_destroy(dtls_srtp->dtls, &dtls_srtp->session);
  }
  dtls_srtp->state = DTLS_SRTP_STATE_INIT;
  dtls_srtp->output_pending = 0;
}

int dtls_srtp_set_remote_fingerprint(DtlsSrtp* dtls_srtp,
                                     const char* fingerprint) {
  if (dtls_srtp == NULL ||
      dtls_srtp_parse_fingerprint(
          fingerprint, dtls_srtp->remote_fingerprint) != 0) {
    return -1;
  }
  dtls_srtp->remote_fingerprint_set = 1;
  if (dtls_srtp->session == NULL) {
    return 0;
  }
  return h2_pal_dtls_session_set_remote_fingerprint(
             dtls_srtp->dtls, dtls_srtp->session,
             dtls_srtp->remote_fingerprint) == H2_PAL_OK
             ? 0
             : -1;
}

int dtls_srtp_handshake(
    DtlsSrtp* dtls_srtp, const uint8_t* datagram, size_t datagram_len) {
  if (dtls_srtp == NULL || dtls_srtp->session == NULL ||
      !dtls_srtp->remote_fingerprint_set) {
    return -1;
  }
  uint64_t now_ms = 0u;
  if (h2_pal_time_get_monotonic_ms(dtls_srtp->time, &now_ms) != H2_PAL_OK) {
    return -1;
  }
  if (dtls_srtp->state == DTLS_SRTP_STATE_INIT) {
    dtls_srtp->state = DTLS_SRTP_STATE_HANDSHAKE;
    dtls_srtp->handshake_deadline_ms =
        now_ms > UINT64_MAX - CONFIG_TLS_READ_TIMEOUT
            ? UINT64_MAX
            : now_ms + CONFIG_TLS_READ_TIMEOUT;
  }
  int complete = 0;
  h2_pal_result_t result = h2_pal_dtls_session_handshake(
      dtls_srtp->dtls, dtls_srtp->session,
      datagram, datagram_len, now_ms,
      dtls_srtp->handshake_deadline_ms, &complete);
  h2_pal_result_t flush_result = h2_pal_dtls_session_flush(
      dtls_srtp->dtls, dtls_srtp->session);
  if (result == H2_PAL_ERR_WOULD_BLOCK || flush_result == H2_PAL_ERR_WOULD_BLOCK ||
      (result == H2_PAL_OK && !complete)) {
    return 1;
  }
  if (result != H2_PAL_OK || flush_result != H2_PAL_OK) {
    return -1;
  }
  return dtls_srtp_configure_srtp(dtls_srtp);
}

void dtls_srtp_reset_session(DtlsSrtp* dtls_srtp) {
  if (dtls_srtp == NULL) {
    return;
  }
  dtls_srtp_deinit(dtls_srtp);
}

h2_pal_result_t dtls_srtp_write(
    DtlsSrtp* dtls_srtp, const uint8_t* buf, size_t len) {
  if (dtls_srtp == NULL || (buf == NULL && len != 0u) || len > INT_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_pal_result_t result =
      h2_pal_dtls_session_write(dtls_srtp->dtls, dtls_srtp->session, buf, len);
  if (result != H2_PAL_OK && result != H2_PAL_ERR_WOULD_BLOCK) {
    H2_PEER_LOGE(dtls_srtp->log, "DTLS write failed %d len %u", (int)result,
                 (unsigned int)len);
  }
  if (result != H2_PAL_OK) {
    return result;
  }
  result = h2_pal_dtls_session_flush(
      dtls_srtp->dtls, dtls_srtp->session);
  if (result == H2_PAL_ERR_WOULD_BLOCK) {
    dtls_srtp->output_pending = 1;
    return H2_PAL_OK;
  }
  if (result == H2_PAL_OK) {
    dtls_srtp->output_pending = 0;
  } else {
    H2_PEER_LOGE(dtls_srtp->log, "DTLS flush failed %d len %u", (int)result,
                 (unsigned int)len);
  }
  return result;
}

h2_pal_result_t dtls_srtp_flush_pending(DtlsSrtp* dtls_srtp) {
  if (dtls_srtp == NULL || dtls_srtp->dtls == NULL ||
      dtls_srtp->session == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (!dtls_srtp->output_pending) {
    return H2_PAL_OK;
  }
  h2_pal_result_t result = h2_pal_dtls_session_flush(
      dtls_srtp->dtls, dtls_srtp->session);
  if (result == H2_PAL_OK) {
    dtls_srtp->output_pending = 0;
  }
  return result;
}

int dtls_srtp_read(DtlsSrtp* dtls_srtp,
                   const uint8_t* datagram, size_t datagram_len,
                   uint8_t* buf, size_t len) {
  if (dtls_srtp == NULL || buf == NULL || len > INT_MAX) {
    return -1;
  }
  dtls_srtp->plaintext_out = buf;
  dtls_srtp->plaintext_capacity = len;
  dtls_srtp->plaintext_len = 0u;
  dtls_srtp->plaintext_result = H2_PAL_OK;
  h2_pal_result_t result = h2_pal_dtls_session_consume_datagram(
      dtls_srtp->dtls, dtls_srtp->session, datagram, datagram_len);
  dtls_srtp->plaintext_out = NULL;
  if (result != H2_PAL_OK) {
    return result == H2_PAL_ERR_CLOSED ? H2_PAL_ERR_CLOSED : -1;
  }
  if (dtls_srtp->plaintext_result != H2_PAL_OK ||
      dtls_srtp->plaintext_len > INT_MAX) {
    return -1;
  }
  return (int)dtls_srtp->plaintext_len;
}

int dtls_srtp_probe(const uint8_t* buf) {
  return buf != NULL && buf[0] >= 20u && buf[0] <= 63u;
}

void dtls_srtp_decrypt_rtp_packet(DtlsSrtp* dtls_srtp, uint8_t* packet,
                                  int* bytes) {
  if (dtls_srtp == NULL || bytes == NULL || *bytes < 0) {
    return;
  }
  size_t packet_len = (size_t)*bytes;
  if (h2_libsrtp_unprotect_rtp(
          dtls_srtp->srtp_in, packet, CONFIG_MTU, &packet_len) != H2_PAL_OK ||
      packet_len > INT_MAX) {
    *bytes = 0;
    return;
  }
  *bytes = (int)packet_len;
}

void dtls_srtp_decrypt_rtcp_packet(DtlsSrtp* dtls_srtp, uint8_t* packet,
                                   int* bytes) {
  if (dtls_srtp == NULL || bytes == NULL || *bytes < 0) {
    return;
  }
  size_t packet_len = (size_t)*bytes;
  if (h2_libsrtp_unprotect_rtcp(
          dtls_srtp->srtp_in, packet, CONFIG_MTU, &packet_len) != H2_PAL_OK ||
      packet_len > INT_MAX) {
    *bytes = 0;
    return;
  }
  *bytes = (int)packet_len;
}

int dtls_srtp_encrypt_rtp_packet(DtlsSrtp* dtls_srtp, uint8_t* packet,
                                 int* bytes) {
  if (dtls_srtp == NULL || bytes == NULL || *bytes < 0) {
    return -1;
  }
  size_t packet_len = (size_t)*bytes;
  if (h2_libsrtp_protect_rtp(
          dtls_srtp->srtp_out, packet, CONFIG_MTU, &packet_len) != H2_PAL_OK ||
      packet_len > INT_MAX) {
    return -1;
  }
  *bytes = (int)packet_len;
  return 0;
}

void dtls_srtp_encrypt_rctp_packet(DtlsSrtp* dtls_srtp, uint8_t* packet,
                                   int* bytes) {
  if (dtls_srtp == NULL || bytes == NULL || *bytes < 0) {
    return;
  }
  size_t packet_len = (size_t)*bytes;
  if (h2_libsrtp_protect_rtcp(
          dtls_srtp->srtp_out, packet, CONFIG_MTU, &packet_len) != H2_PAL_OK ||
      packet_len > INT_MAX) {
    *bytes = 0;
    return;
  }
  *bytes = (int)packet_len;
}
