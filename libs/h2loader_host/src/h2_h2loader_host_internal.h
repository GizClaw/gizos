#ifndef H2_H2LOADER_HOST_INTERNAL_H
#define H2_H2LOADER_HOST_INTERNAL_H

#include "h2_h2loader_host.h"

#include <stddef.h>
#include <stdint.h>

typedef struct h2_h2loader_host_sha256 {
    uint32_t state[8];
    uint64_t total_bytes;
    uint8_t block[64];
    size_t block_len;
} h2_h2loader_host_sha256_t;

void h2_h2loader_host_sha256_init(h2_h2loader_host_sha256_t *sha);
void h2_h2loader_host_sha256_update(
    h2_h2loader_host_sha256_t *sha,
    const uint8_t *data,
    size_t len);
void h2_h2loader_host_sha256_finish(
    h2_h2loader_host_sha256_t *sha,
    uint8_t out[32]);
void h2_h2loader_host_sha256_hex(
    const uint8_t digest[32],
    char out[H2_H2LOADER_HOST_SHA256_HEX_LEN + 1u]);

int h2_h2loader_host_copy_text(
    char *out,
    size_t out_size,
    const char *value,
    size_t value_len);
int h2_h2loader_host_is_safe_identity(const char *value);
int h2_h2loader_host_is_sha256(const char *value);
int h2_h2loader_host_is_safe_resource_name(const char *value);

typedef struct h2_h2loader_host_command_contract {
    char line[1024];
    const char *marker;
    const char *success_token;
    const char *accepted_disconnect_token;
    uint8_t lifecycle_transition;
    uint8_t marker_is_success;
} h2_h2loader_host_command_contract_t;

h2_pal_result_t h2_h2loader_host_command_contract(
    const h2_h2loader_host_command_request_t *request,
    h2_h2loader_host_command_contract_t *out_contract);

h2_h2loader_host_command_terminal_t h2_h2loader_host_command_parse_terminal(
    const uint8_t *response,
    size_t response_len,
    const h2_h2loader_host_command_contract_t *contract);

typedef h2_pal_result_t (*h2_h2loader_host_command_write_fn)(
    void *transport,
    const char *line);

typedef h2_pal_result_t (*h2_h2loader_host_command_read_fn)(
    void *transport,
    const char *marker,
    uint8_t *response,
    size_t response_size,
    size_t *out_response_len,
    h2_h2loader_host_command_output_fn on_output,
    void *output_user);

typedef h2_pal_result_t (*h2_h2loader_host_command_finish_fn)(
    void *transport);

h2_pal_result_t h2_h2loader_host_command_execute_transport(
    void *transport,
    h2_h2loader_host_command_write_fn write_command,
    h2_h2loader_host_command_read_fn read_response,
    h2_h2loader_host_command_finish_fn finish_response,
    const h2_h2loader_host_command_request_t *request,
    h2_h2loader_host_command_result_t *out_result);

#endif
