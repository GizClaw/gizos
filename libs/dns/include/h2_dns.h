#ifndef H2_DNS_H
#define H2_DNS_H

#include "h2/pal/os/h2_pal_crypto.h"
#include "h2/pal/net/h2_pal_net.h"
#include "h2/pal/os/h2_pal_time.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_DNS_PORT 53u
#define H2_DNS_MAX_PACKET_SIZE 512u
#define H2_DNS_MAX_NAME_SIZE 253u

typedef enum h2_dns_record_type {
    H2_DNS_RECORD_A = 1,
    H2_DNS_RECORD_AAAA = 28,
} h2_dns_record_type_t;

typedef enum h2_dns_result {
    H2_DNS_OK = 0,
    H2_DNS_ERR_INVALID_ARG = -1,
    H2_DNS_ERR_TRUNCATED = -2,
    H2_DNS_ERR_MALFORMED = -3,
    H2_DNS_ERR_TXID_MISMATCH = -4,
    H2_DNS_ERR_NXDOMAIN = -5,
    H2_DNS_ERR_SERVER_FAILURE = -6,
    H2_DNS_ERR_NO_ANSWER = -7,
    H2_DNS_ERR_UNSUPPORTED = -8,
    H2_DNS_ERR_TRANSPORT = -9,
    H2_DNS_ERR_TIMEOUT = -10,
    H2_DNS_ERR_NO_SPACE = -11,
} h2_dns_result_t;

typedef struct h2_dns_answer {
    h2_dns_record_type_t type;
    h2_pal_net_addr_t addr;
} h2_dns_answer_t;

typedef struct h2_dns_client_config {
    const h2_pal_net_api_t *net;
    const h2_pal_crypto_api_t *crypto;
    const h2_pal_time_api_t *time;
    h2_pal_net_addr_t server;
    const h2_pal_net_bind_t *bind;
    uint32_t timeout_ms;
    uint8_t retries;
} h2_dns_client_config_t;

typedef struct h2_dns_query {
    const char *name;
    h2_dns_record_type_t type;
    uint8_t probe_only;
    h2_dns_answer_t *answers;
    size_t max_answers;
    size_t *out_count;
} h2_dns_query_t;

int h2_dns_encode_query(
    const char *name,
    h2_dns_record_type_t type,
    uint16_t txid,
    uint8_t *out_packet,
    size_t packet_cap,
    size_t *out_len);

int h2_dns_parse_response(
    const uint8_t *packet,
    size_t packet_len,
    uint16_t expected_txid,
    const char *expected_name,
    h2_dns_record_type_t type,
    h2_dns_answer_t *out_answers,
    size_t max_answers,
    size_t *out_count);

int h2_dns_query(const h2_dns_client_config_t *config, const h2_dns_query_t *query);

#ifdef __cplusplus
}
#endif

#endif
