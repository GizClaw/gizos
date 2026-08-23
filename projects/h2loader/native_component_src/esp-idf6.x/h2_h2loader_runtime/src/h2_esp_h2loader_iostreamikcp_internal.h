#ifndef H2_ESP_H2LOADER_IOSTREAMIKCP_INTERNAL_H
#define H2_ESP_H2LOADER_IOSTREAMIKCP_INTERNAL_H

#include "h2_command.h"
#include "h2_esp_platform_core.h"
#include "h2_iostreamikcp.h"

typedef struct h2_esp_h2loader_command_transport {
    h2_iostreamikcp_io_t physical_io;
    h2_iostreamikcp_filter_t filter;
    h2_iostreamikcp_t *stream;
    const h2_pal_mem_api_t *allocator;
    uint32_t conv;
    uint32_t pending_conv;
    uint32_t write_timeout_ms;
    uint32_t receive_window;
    int replacement_pending;
} h2_esp_h2loader_command_transport_t;

h2_pal_result_t h2_esp_h2loader_console_init(void);
h2_pal_result_t h2_esp_h2loader_command_transport_init(
    h2_esp_h2loader_command_transport_t *transport,
    const h2_pal_mem_api_t *allocator);
void h2_esp_h2loader_command_transport_deinit(
    h2_esp_h2loader_command_transport_t *transport);
h2_command_io_api_t h2_esp_h2loader_command_transport_io(
    h2_esp_h2loader_command_transport_t *transport);
h2_pal_result_t h2_esp_h2loader_command_transport_poll_session(
    h2_esp_h2loader_command_transport_t *transport,
    uint32_t timeout_ms);
h2_pal_result_t h2_esp_h2loader_command_transport_activate_pending(
    h2_esp_h2loader_command_transport_t *transport);
int h2_esp_h2loader_command_transport_has_session(
    const h2_esp_h2loader_command_transport_t *transport);
int h2_esp_h2loader_command_transport_replacement_pending(
    const h2_esp_h2loader_command_transport_t *transport);

#endif
