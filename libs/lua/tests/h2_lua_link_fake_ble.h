#ifndef H2_LUA_LINK_FAKE_BLE_H
#define H2_LUA_LINK_FAKE_BLE_H

/*
 * Test-only pair of BLE Hosts joined by one "air", for Lua link tests.
 * Device 0 and device 1 each have their own system-event bus; connect, MTU
 * exchange, GATT writes, notifications and disconnects are delivered
 * synchronously to the other device, like a radio that never loses a packet.
 * The GATT table behaves like ESP NimBLE (two retained service slots, one
 * already taken by a foreign service), and scans report each peer address
 * once until they restart, while that foreign service keeps advertising.
 */

#include "h2_pal.h"
#include "h2_runtime.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FAKE_SUBSCRIPTION_MAX 128u
#define FAKE_PERIPHERAL_HANDLE 7u
#define FAKE_CENTRAL_HANDLE 9u
#define FAKE_MTU 247u
/* Like ESP NimBLE: two service slots, never freed once registered. */
#define FAKE_GATT_SERVICE_SLOTS 2u

struct h2_pal_system_event_subscription {
  h2_pal_system_event_type_t type;
  h2_pal_system_event_handler_t handler;
  void *user;
  int used;
};

typedef struct fake_air fake_air_t;

/* Pauses a session after it publishes its terminal event, before task_done.
 * The fake server's unregister callback arms the next PAL mutex unlock on
 * that same thread: server cleanup has no further PAL mutex unlocks. */
typedef struct fake_terminal_gate {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  atomic_int reached;
  int released;
} fake_terminal_gate_t;

typedef struct fake_device {
  fake_air_t *air;
  int index;
  h2_pal_ble_host_api_t ble;
  h2_pal_system_event_api_t events;
  /* Desktop sync with an unlock hook for terminal_gate; Runtimes built on
   * this device's BLE Host use it. */
  h2_pal_sync_api_t sync;
  h2_pal_sync_vtable_t sync_vtable;
  fake_terminal_gate_t *terminal_gate;
  pthread_mutex_t bus_mutex;
  struct h2_pal_system_event_subscription subscriptions[FAKE_SUBSCRIPTION_MAX];
  /* Guarded by air->mutex. */
  int adv_sets;
  int adv_running;
  uint8_t adv_uuid[16];
  size_t adv_uuid_len;
  int scanning;
  /* Controller duplicate filter: one report per peer address per scan. */
  int scan_seen_peer;
  int scan_starts;
  /* Always advertises another service from the same address, like the
   * H2Loader management advertisement in App images. */
  int foreign_adv;
  h2_pal_ble_scan_result_fn scan_cb;
  void *scan_user;
  const h2_pal_ble_gatt_service_t *service;
  uint8_t retained_uuid[FAKE_GATT_SERVICE_SLOTS][16];
  size_t retained_count;
  int registrations;
  int connected;
  uint16_t conn_handle;
  int connects;
  /* Subscriptions the Runtime itself keeps; the link must return to it. */
  int baseline_subscriptions;
} fake_device_t;

struct h2_pal_ble_adv_set {
  fake_device_t *device;
};

struct fake_air {
  pthread_mutex_t mutex;
  fake_device_t devices[2];
};


typedef struct fake_snapshot {
  int adv_sets;
  int adv_running;
  int scanning;
  int registered;
  int connected;
  int subscriptions;
} fake_snapshot_t;

/* Resets the air: both devices idle, each with one foreign service. */
void fake_air_init(fake_air_t *air);
/* Drops the connection as if the radio link were lost (no BYE). */
void fake_drop_link(fake_air_t *air);
fake_snapshot_t fake_snapshot(fake_device_t *device);
/* Records the subscriptions the Runtime itself keeps on the device bus. */
void fake_set_baseline(fake_device_t *device);
/* No advertising, scan, attached service, connection or extra subscription. */
int fake_is_released(fake_device_t *device);
/* A Runtime on desktop OS providers with this BLE Host and event bus and
 * no Wi-Fi or network provider. */
h2_runtime_t *fake_create_runtime(const h2_pal_ble_host_api_t *ble,
                                  const h2_pal_system_event_api_t *events);

#ifdef __cplusplus
}
#endif

#endif
