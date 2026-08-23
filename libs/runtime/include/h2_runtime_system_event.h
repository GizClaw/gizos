#ifndef H2_RUNTIME_SYSTEM_EVENT_H
#define H2_RUNTIME_SYSTEM_EVENT_H

/* Scope: Runtime-owned system event taxonomy and payload schemas. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_RUNTIME_SYSTEM_WIFI_SSID_MAX 32u
#define H2_RUNTIME_SYSTEM_WIFI_BSSID_LEN 6u
#define H2_RUNTIME_SYSTEM_WIFI_MAC_LEN 6u
#define H2_RUNTIME_SYSTEM_BLE_ADDR_LEN 6u
#define H2_RUNTIME_SYSTEM_BLE_ATT_MAX_VALUE_LEN 514u
#define H2_RUNTIME_SYSTEM_MODEM_PHONE_NUMBER_MAX 32u
#define H2_RUNTIME_SYSTEM_NETIF_NAME_MAX 16u

#include "h2_runtime_event.h"

typedef int32_t h2_runtime_system_result_t;

typedef enum h2_runtime_system_netif_kind {
    H2_RUNTIME_SYSTEM_NETIF_KIND_UNKNOWN = 0,
    H2_RUNTIME_SYSTEM_NETIF_KIND_LOOPBACK,
    H2_RUNTIME_SYSTEM_NETIF_KIND_WIFI_STA,
    H2_RUNTIME_SYSTEM_NETIF_KIND_WIFI_AP,
    H2_RUNTIME_SYSTEM_NETIF_KIND_MODEM_DATA,
    H2_RUNTIME_SYSTEM_NETIF_KIND_ETHERNET,
} h2_runtime_system_netif_kind_t;

typedef struct h2_runtime_system_netif_ref {
    h2_runtime_system_netif_kind_t kind;
    uint32_t id;
    char name[H2_RUNTIME_SYSTEM_NETIF_NAME_MAX];
    uint8_t id_valid;
    uint8_t name_valid;
} h2_runtime_system_netif_ref_t;

typedef struct h2_runtime_system_event_netif_default_changed {
    h2_runtime_system_netif_ref_t previous;
    h2_runtime_system_netif_ref_t current;
    uint8_t previous_valid;
    uint8_t current_valid;
} h2_runtime_system_event_netif_default_changed_t;

typedef enum h2_runtime_system_gpio_irq_trigger {
    H2_RUNTIME_SYSTEM_GPIO_IRQ_TRIGGER_UNKNOWN = 0,
    H2_RUNTIME_SYSTEM_GPIO_IRQ_TRIGGER_RISING,
    H2_RUNTIME_SYSTEM_GPIO_IRQ_TRIGGER_FALLING,
    H2_RUNTIME_SYSTEM_GPIO_IRQ_TRIGGER_BOTH,
    H2_RUNTIME_SYSTEM_GPIO_IRQ_TRIGGER_HIGH,
    H2_RUNTIME_SYSTEM_GPIO_IRQ_TRIGGER_LOW,
} h2_runtime_system_gpio_irq_trigger_t;

typedef enum h2_runtime_system_wifi_sta_status {
    H2_RUNTIME_SYSTEM_WIFI_STA_STATUS_UNKNOWN = 0,
    H2_RUNTIME_SYSTEM_WIFI_STA_STATUS_IDLE,
    H2_RUNTIME_SYSTEM_WIFI_STA_STATUS_SCANNING,
    H2_RUNTIME_SYSTEM_WIFI_STA_STATUS_CONNECTING,
    H2_RUNTIME_SYSTEM_WIFI_STA_STATUS_CONNECTED,
    H2_RUNTIME_SYSTEM_WIFI_STA_STATUS_GOT_IP,
    H2_RUNTIME_SYSTEM_WIFI_STA_STATUS_DISCONNECTED,
    H2_RUNTIME_SYSTEM_WIFI_STA_STATUS_FAILED,
} h2_runtime_system_wifi_sta_status_t;

typedef enum h2_runtime_system_wifi_ap_status {
    H2_RUNTIME_SYSTEM_WIFI_AP_STATUS_UNKNOWN = 0,
    H2_RUNTIME_SYSTEM_WIFI_AP_STATUS_STOPPED,
    H2_RUNTIME_SYSTEM_WIFI_AP_STATUS_STARTING,
    H2_RUNTIME_SYSTEM_WIFI_AP_STATUS_STARTED,
    H2_RUNTIME_SYSTEM_WIFI_AP_STATUS_STOPPING,
} h2_runtime_system_wifi_ap_status_t;

typedef enum h2_runtime_system_wifi_security {
    H2_RUNTIME_SYSTEM_WIFI_SECURITY_UNKNOWN = 0,
    H2_RUNTIME_SYSTEM_WIFI_SECURITY_OPEN,
    H2_RUNTIME_SYSTEM_WIFI_SECURITY_WEP,
    H2_RUNTIME_SYSTEM_WIFI_SECURITY_WPA,
    H2_RUNTIME_SYSTEM_WIFI_SECURITY_WPA2,
    H2_RUNTIME_SYSTEM_WIFI_SECURITY_WPA3,
    H2_RUNTIME_SYSTEM_WIFI_SECURITY_WPA_WPA2,
    H2_RUNTIME_SYSTEM_WIFI_SECURITY_WPA2_WPA3,
    H2_RUNTIME_SYSTEM_WIFI_SECURITY_ENTERPRISE,
} h2_runtime_system_wifi_security_t;

typedef enum h2_runtime_system_ble_addr_type {
    H2_RUNTIME_SYSTEM_BLE_ADDR_TYPE_UNKNOWN = 0,
    H2_RUNTIME_SYSTEM_BLE_ADDR_TYPE_PUBLIC,
    H2_RUNTIME_SYSTEM_BLE_ADDR_TYPE_RANDOM,
    H2_RUNTIME_SYSTEM_BLE_ADDR_TYPE_PUBLIC_IDENTITY,
    H2_RUNTIME_SYSTEM_BLE_ADDR_TYPE_RANDOM_IDENTITY,
} h2_runtime_system_ble_addr_type_t;

typedef enum h2_runtime_system_ble_role {
    H2_RUNTIME_SYSTEM_BLE_ROLE_UNKNOWN = 0,
    H2_RUNTIME_SYSTEM_BLE_ROLE_PERIPHERAL,
    H2_RUNTIME_SYSTEM_BLE_ROLE_CENTRAL,
} h2_runtime_system_ble_role_t;

typedef enum h2_runtime_system_ble_subscribe_mode {
    H2_RUNTIME_SYSTEM_BLE_SUBSCRIBE_MODE_NOTIFY = 0,
    H2_RUNTIME_SYSTEM_BLE_SUBSCRIBE_MODE_INDICATE,
} h2_runtime_system_ble_subscribe_mode_t;

typedef enum h2_runtime_system_modem_sim_state {
    H2_RUNTIME_SYSTEM_MODEM_SIM_UNKNOWN = 0,
    H2_RUNTIME_SYSTEM_MODEM_SIM_ABSENT,
    H2_RUNTIME_SYSTEM_MODEM_SIM_LOCKED,
    H2_RUNTIME_SYSTEM_MODEM_SIM_READY,
} h2_runtime_system_modem_sim_state_t;

typedef enum h2_runtime_system_modem_registration_state {
    H2_RUNTIME_SYSTEM_MODEM_REGISTRATION_UNKNOWN = 0,
    H2_RUNTIME_SYSTEM_MODEM_REGISTRATION_OFFLINE,
    H2_RUNTIME_SYSTEM_MODEM_REGISTRATION_SEARCHING,
    H2_RUNTIME_SYSTEM_MODEM_REGISTRATION_DENIED,
    H2_RUNTIME_SYSTEM_MODEM_REGISTRATION_HOME,
    H2_RUNTIME_SYSTEM_MODEM_REGISTRATION_ROAMING,
} h2_runtime_system_modem_registration_state_t;

typedef enum h2_runtime_system_modem_packet_state {
    H2_RUNTIME_SYSTEM_MODEM_PACKET_UNKNOWN = 0,
    H2_RUNTIME_SYSTEM_MODEM_PACKET_DETACHED,
    H2_RUNTIME_SYSTEM_MODEM_PACKET_ATTACHING,
    H2_RUNTIME_SYSTEM_MODEM_PACKET_ATTACHED,
    H2_RUNTIME_SYSTEM_MODEM_PACKET_CONNECTING,
    H2_RUNTIME_SYSTEM_MODEM_PACKET_CONNECTED,
} h2_runtime_system_modem_packet_state_t;

typedef enum h2_runtime_system_modem_rat {
    H2_RUNTIME_SYSTEM_MODEM_RAT_UNKNOWN = 0,
    H2_RUNTIME_SYSTEM_MODEM_RAT_GSM,
    H2_RUNTIME_SYSTEM_MODEM_RAT_GPRS,
    H2_RUNTIME_SYSTEM_MODEM_RAT_EDGE,
    H2_RUNTIME_SYSTEM_MODEM_RAT_WCDMA,
    H2_RUNTIME_SYSTEM_MODEM_RAT_HSPA,
    H2_RUNTIME_SYSTEM_MODEM_RAT_LTE,
    H2_RUNTIME_SYSTEM_MODEM_RAT_LTE_M,
    H2_RUNTIME_SYSTEM_MODEM_RAT_NB_IOT,
    H2_RUNTIME_SYSTEM_MODEM_RAT_NR5G,
} h2_runtime_system_modem_rat_t;

typedef enum h2_runtime_system_modem_data_state {
    H2_RUNTIME_SYSTEM_MODEM_DATA_CLOSED = 0,
    H2_RUNTIME_SYSTEM_MODEM_DATA_OPENING,
    H2_RUNTIME_SYSTEM_MODEM_DATA_OPEN,
    H2_RUNTIME_SYSTEM_MODEM_DATA_CLOSING,
} h2_runtime_system_modem_data_state_t;

typedef enum h2_runtime_system_modem_call_direction {
    H2_RUNTIME_SYSTEM_MODEM_CALL_DIRECTION_UNKNOWN = 0,
    H2_RUNTIME_SYSTEM_MODEM_CALL_DIRECTION_INCOMING,
    H2_RUNTIME_SYSTEM_MODEM_CALL_DIRECTION_OUTGOING,
} h2_runtime_system_modem_call_direction_t;

typedef enum h2_runtime_system_modem_call_state {
    H2_RUNTIME_SYSTEM_MODEM_CALL_STATE_IDLE = 0,
    H2_RUNTIME_SYSTEM_MODEM_CALL_STATE_INCOMING,
    H2_RUNTIME_SYSTEM_MODEM_CALL_STATE_DIALING,
    H2_RUNTIME_SYSTEM_MODEM_CALL_STATE_ALERTING,
    H2_RUNTIME_SYSTEM_MODEM_CALL_STATE_ACTIVE,
    H2_RUNTIME_SYSTEM_MODEM_CALL_STATE_HELD,
    H2_RUNTIME_SYSTEM_MODEM_CALL_STATE_WAITING,
    H2_RUNTIME_SYSTEM_MODEM_CALL_STATE_ENDED,
} h2_runtime_system_modem_call_state_t;

typedef struct h2_runtime_system_wifi_ip_info {
    uint32_t ip4;
    uint32_t netmask4;
    uint32_t gateway4;
} h2_runtime_system_wifi_ip_info_t;

typedef struct h2_runtime_system_event_gpio_irq {
    h2_runtime_system_gpio_irq_trigger_t trigger;
} h2_runtime_system_event_gpio_irq_t;

typedef struct h2_runtime_system_event_wifi_sta {
    h2_runtime_system_wifi_sta_status_t status;
    char ssid[H2_RUNTIME_SYSTEM_WIFI_SSID_MAX + 1u];
    size_t ssid_len;
    uint8_t bssid[H2_RUNTIME_SYSTEM_WIFI_BSSID_LEN];
    uint8_t bssid_set;
    uint8_t channel;
    int32_t rssi;
    h2_runtime_system_wifi_ip_info_t ip;
    uint8_t ip_valid;
    int32_t disconnect_reason;
} h2_runtime_system_event_wifi_sta_t;

typedef struct h2_runtime_system_event_wifi_ap {
    h2_runtime_system_wifi_ap_status_t status;
    char ssid[H2_RUNTIME_SYSTEM_WIFI_SSID_MAX + 1u];
    size_t ssid_len;
    uint8_t channel;
    uint8_t max_clients;
    size_t client_count;
    h2_runtime_system_wifi_security_t security;
    uint8_t hidden;
} h2_runtime_system_event_wifi_ap_t;

typedef struct h2_runtime_system_event_wifi_ap_client {
    uint8_t mac[H2_RUNTIME_SYSTEM_WIFI_MAC_LEN];
    int32_t rssi;
    h2_runtime_system_wifi_ip_info_t lease;
    uint8_t lease_valid;
    int32_t station_id;
} h2_runtime_system_event_wifi_ap_client_t;

typedef struct h2_runtime_system_ble_addr {
    uint8_t value[H2_RUNTIME_SYSTEM_BLE_ADDR_LEN];
    h2_runtime_system_ble_addr_type_t type;
} h2_runtime_system_ble_addr_t;

typedef struct h2_runtime_system_event_ble_connection {
    uint16_t conn_handle;
    h2_runtime_system_ble_role_t role;
    h2_runtime_system_ble_addr_t peer_addr;
    uint16_t mtu;
} h2_runtime_system_event_ble_connection_t;

typedef struct h2_runtime_system_event_ble_advertising_set {
    /** Opaque identity of the advertising set that completed the operation. */
    void *set;
    h2_runtime_system_result_t result;
} h2_runtime_system_event_ble_advertising_set_t;

typedef struct h2_runtime_system_event_ble_connection_params {
    uint16_t interval_min_ms;
    uint16_t interval_max_ms;
    uint16_t latency;
    uint16_t supervision_timeout_ms;
} h2_runtime_system_event_ble_connection_params_t;

typedef struct h2_runtime_system_event_ble_disconnected {
    uint16_t conn_handle;
    h2_runtime_system_ble_addr_t peer_addr;
    int32_t reason;
} h2_runtime_system_event_ble_disconnected_t;

typedef struct h2_runtime_system_event_ble_mtu {
    uint16_t conn_handle;
    uint16_t mtu;
} h2_runtime_system_event_ble_mtu_t;

typedef struct h2_runtime_system_event_ble_subscription {
    uint16_t conn_handle;
    uint16_t value_handle;
    h2_runtime_system_ble_subscribe_mode_t mode;
    bool enabled;
} h2_runtime_system_event_ble_subscription_t;

typedef struct h2_runtime_system_event_ble_gatt_client_value {
    uint16_t conn_handle;
    uint16_t attr_handle;
    size_t value_len;
    uint8_t value[H2_RUNTIME_SYSTEM_BLE_ATT_MAX_VALUE_LEN];
} h2_runtime_system_event_ble_gatt_client_value_t;

typedef struct h2_runtime_system_event_modem_error {
    h2_runtime_system_result_t result;
    int32_t vendor_code;
} h2_runtime_system_event_modem_error_t;

typedef struct h2_runtime_system_event_modem_sim {
    uint32_t capabilities;
    h2_runtime_system_modem_sim_state_t state;
    h2_runtime_system_modem_rat_t rat;
} h2_runtime_system_event_modem_sim_t;

typedef struct h2_runtime_system_event_modem_registration {
    uint32_t capabilities;
    h2_runtime_system_modem_registration_state_t state;
    h2_runtime_system_modem_rat_t rat;
} h2_runtime_system_event_modem_registration_t;

typedef struct h2_runtime_system_event_modem_packet {
    uint32_t capabilities;
    h2_runtime_system_modem_packet_state_t state;
    h2_runtime_system_modem_rat_t rat;
} h2_runtime_system_event_modem_packet_t;

typedef struct h2_runtime_system_event_modem_signal {
    int32_t rssi_dbm;
    int32_t ber;
    h2_runtime_system_modem_rat_t rat;
} h2_runtime_system_event_modem_signal_t;

typedef struct h2_runtime_system_event_modem_data {
    h2_runtime_system_modem_data_state_t state;
    uint32_t ip4;
    uint32_t dns1_ip4;
    uint32_t dns2_ip4;
    uint8_t ip4_valid;
    h2_runtime_system_result_t last_error;
} h2_runtime_system_event_modem_data_t;

typedef struct h2_runtime_system_event_modem_call {
    int32_t call_id;
    h2_runtime_system_modem_call_direction_t direction;
    h2_runtime_system_modem_call_state_t state;
    char number[H2_RUNTIME_SYSTEM_MODEM_PHONE_NUMBER_MAX + 1u];
    int32_t end_reason;
} h2_runtime_system_event_modem_call_t;

#ifdef __cplusplus
}
#endif

#endif
