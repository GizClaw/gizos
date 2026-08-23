#ifndef FAKE_MQTT_PLATFORM_H
#define FAKE_MQTT_PLATFORM_H

#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/net/h2_pal_net.h"
#include "h2/pal/os/h2_pal_time.h"

#include <stddef.h>
#include <stdint.h>

typedef struct fake_mqtt_platform {
    h2_pal_mem_api_t allocator;
    h2_pal_net_api_t net;
    h2_pal_time_api_t time;
    uint8_t rx[2048];
    size_t rx_pos;
    size_t rx_len;
    uint8_t tx[2048];
    size_t tx_len;
    size_t tx_scan_pos;
    uint64_t now_ms;
    int socket_open;
    int close_count;
    int fail_connect;
    int timeout_connect;
    int tls_supported;
    int tls_verify_fail;
    int tls_called;
    int auto_respond;
} fake_mqtt_platform_t;

void fake_mqtt_platform_init(fake_mqtt_platform_t *fake);
void fake_mqtt_platform_push_rx(fake_mqtt_platform_t *fake, const uint8_t *data, size_t len);
void fake_mqtt_platform_push_publish(fake_mqtt_platform_t *fake, const char *topic, const uint8_t *payload, size_t payload_len);

#endif
