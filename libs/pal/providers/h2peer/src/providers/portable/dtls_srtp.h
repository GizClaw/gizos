#ifndef DTLS_SRTP_H_
#define DTLS_SRTP_H_

#include <stddef.h>
#include <stdint.h>

#include "h2/pal/net/h2_pal_dtls.h"
#include "h2/pal/os/h2_pal_log.h"
#include "h2/pal/os/h2_pal_time.h"
#include "h2_libsrtp.h"

#define DTLS_SRTP_FINGERPRINT_LENGTH 96

typedef enum DtlsSrtpRole {
  DTLS_SRTP_ROLE_CLIENT,
  DTLS_SRTP_ROLE_SERVER
} DtlsSrtpRole;

typedef enum DtlsSrtpState {
  DTLS_SRTP_STATE_INIT,
  DTLS_SRTP_STATE_HANDSHAKE,
  DTLS_SRTP_STATE_CONNECTED
} DtlsSrtpState;

typedef struct DtlsSrtp {
  const h2_pal_log_api_t *log;
  const h2_pal_dtls_api_t *dtls;
  const h2_pal_time_api_t *time;
  h2_pal_dtls_session_t *session;
  h2_libsrtp_session_t* srtp_in;
  h2_libsrtp_session_t* srtp_out;

  int (*packet_send)(void* ctx, const unsigned char* buf, size_t len);
  DtlsSrtpRole role;
  DtlsSrtpState state;
  char local_fingerprint[DTLS_SRTP_FINGERPRINT_LENGTH];
  uint8_t remote_fingerprint[H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE];
  int remote_fingerprint_set;
  uint64_t handshake_deadline_ms;
  uint8_t* plaintext_out;
  size_t plaintext_capacity;
  size_t plaintext_len;
  h2_pal_result_t plaintext_result;
  int output_pending;
  void* user_data;
} DtlsSrtp;

int dtls_srtp_init(DtlsSrtp *dtls_srtp, DtlsSrtpRole role, void *user_data,
                   const h2_pal_log_api_t *log, const h2_pal_dtls_api_t *dtls,
                   const h2_pal_time_api_t *time);
void dtls_srtp_deinit(DtlsSrtp *dtls_srtp);
int dtls_srtp_set_remote_fingerprint(DtlsSrtp* dtls_srtp,
                                     const char* fingerprint);
int dtls_srtp_handshake(
    DtlsSrtp* dtls_srtp, const uint8_t* datagram, size_t datagram_len);
void dtls_srtp_reset_session(DtlsSrtp* dtls_srtp);
h2_pal_result_t dtls_srtp_write(
    DtlsSrtp* dtls_srtp, const uint8_t* buf, size_t len);
h2_pal_result_t dtls_srtp_flush_pending(DtlsSrtp* dtls_srtp);
int dtls_srtp_read(DtlsSrtp* dtls_srtp,
                   const uint8_t* datagram, size_t datagram_len,
                   uint8_t* buf, size_t len);
int dtls_srtp_probe(const uint8_t* buf);
void dtls_srtp_decrypt_rtp_packet(DtlsSrtp* dtls_srtp, uint8_t* packet,
                                  int* bytes);
void dtls_srtp_decrypt_rtcp_packet(DtlsSrtp* dtls_srtp, uint8_t* packet,
                                   int* bytes);
int dtls_srtp_encrypt_rtp_packet(DtlsSrtp* dtls_srtp, uint8_t* packet,
                                 int* bytes);
void dtls_srtp_encrypt_rctp_packet(DtlsSrtp* dtls_srtp, uint8_t* packet,
                                   int* bytes);

#endif  // DTLS_SRTP_H_
