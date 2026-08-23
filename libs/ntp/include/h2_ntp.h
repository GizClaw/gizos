#ifndef H2_NTP_H
#define H2_NTP_H

#include "h2/pal/net/h2_pal_net.h"
#include "h2/pal/os/h2_pal_time.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_NTP_PORT 123u
#define H2_NTP_PACKET_SIZE 48u
#define H2_NTP_UNIX_EPOCH_DELTA 2208988800u

typedef enum h2_ntp_result {
    H2_NTP_OK = 0,
    H2_NTP_OK_TIME_SET_UNSUPPORTED = 1,
    H2_NTP_ERR_INVALID_ARG = -1,
    H2_NTP_ERR_TRANSPORT = -2,
    H2_NTP_ERR_TIMEOUT = -3,
    H2_NTP_ERR_MALFORMED = -4,
    H2_NTP_ERR_UNSYNCED = -5,
    H2_NTP_ERR_TXID_MISMATCH = -6,
    H2_NTP_ERR_UNSUPPORTED = -7,
} h2_ntp_result_t;

typedef struct h2_ntp_client_config {
    const h2_pal_net_api_t *net;
    const h2_pal_time_api_t *time;
    h2_pal_net_addr_t server;
    const h2_pal_net_bind_t *bind;
    uint32_t timeout_ms;
    uint8_t retries;
    uint8_t set_wall_clock;
} h2_ntp_client_config_t;

typedef struct h2_ntp_sync_result {
    uint64_t server_unix_ms;
    uint64_t local_receive_monotonic_ms;
    uint64_t applied_wall_ms;
    uint32_t round_trip_ms;
    int64_t offset_ms;
    uint8_t wall_clock_set;
} h2_ntp_sync_result_t;

int h2_ntp_unix_ms_to_timestamp(uint64_t unix_ms, uint32_t *out_seconds, uint32_t *out_fraction);
int h2_ntp_timestamp_to_unix_ms(uint32_t seconds, uint32_t fraction, uint64_t *out_unix_ms);
int h2_ntp_build_request(uint64_t unix_ms, uint8_t out_packet[H2_NTP_PACKET_SIZE]);
int h2_ntp_parse_response(
    const uint8_t packet[H2_NTP_PACKET_SIZE],
    uint64_t expected_originate_unix_ms,
    uint64_t local_receive_wall_ms,
    uint64_t local_receive_monotonic_ms,
    h2_ntp_sync_result_t *out_result);
int h2_ntp_sync(const h2_ntp_client_config_t *config, h2_ntp_sync_result_t *out_result);

#ifdef __cplusplus
}
#endif

#endif
