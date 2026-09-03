#include "h2_bloomspeaker_engine.h"

#include "h2_bloomspeaker_audio.h"
#include "h2_bloomspeaker_pairing.h"
#include "h2_bloomspeaker_protocol.h"
#include "h2_bloomspeaker_task_names.h"

#include "h2/pal/os/h2_pal_crypto.h"
#include "h2/pal/os/h2_pal_log.h"
#include "h2_bleikcp.h"
#include "h2_bleikcp_task_names.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define H2_BLOOMSPEAKER_BLE_POLL_MS 20u
#define H2_BLOOMSPEAKER_SETUP_TIMEOUT_MS 6000u
#define H2_BLOOMSPEAKER_PAIR_TIMEOUT_MS 30000u
#define H2_BLOOMSPEAKER_STREAM_TIMEOUT_MS 50u
#define H2_BLOOMSPEAKER_CLAIM_TIMEOUT_MS 2200u
#define H2_BLOOMSPEAKER_DISCONNECT_ANIMATION_MS 420u
#define H2_BLOOMSPEAKER_STREAM_WINDOW 8u
#define H2_BLOOMSPEAKER_STREAM_BUFFER_SIZE 4096u
#define H2_BLOOMSPEAKER_CODEC_TASK_STACK_SIZE (32u * 1024u)
#define H2_BLOOMSPEAKER_SYSTEM_EVENT_SUBSCRIPTION_COUNT 2u

static const uint8_t s_service_uuid[] = {0xb7u, 0xb0u};
static const uint8_t s_tx_uuid[] = {0xb8u, 0xb0u};
static const uint8_t s_rx_uuid[] = {0xb9u, 0xb0u};

typedef struct h2_bloomspeaker_observed_address {
  uint64_t device_tag;
  uint64_t last_seen_ms;
  h2_pal_ble_addr_t address;
  bool occupied;
} h2_bloomspeaker_observed_address_t;

struct h2_bloomspeaker_engine {
  h2_runtime_t *runtime;
  h2_bloomspeaker_controller_t *controller;
  h2_bloomspeaker_audio_t *audio;
  h2_pal_mutex_t *pairing_mutex;
  h2_pal_task_t *task;
  h2_pal_system_event_subscription_t
      *system_event_subscriptions[H2_BLOOMSPEAKER_SYSTEM_EVENT_SUBSCRIPTION_COUNT];
  _Atomic int stop;
  h2_bloomspeaker_pairing_t pairing;
  h2_bloomspeaker_observed_address_t
      addresses[H2_BLOOMSPEAKER_PAIRING_MAX_CANDIDATES];
  h2_pal_ble_adv_set_t *advertising_set;
  h2_bleikcp_server_t *server;
  h2_pal_ble_addr_t peer_address;
  h2_bloomspeaker_pairing_role_t role;
  uint64_t peer_tag;
  uint32_t peer_epoch;
  uint32_t pairing_passkey;
  _Atomic uint16_t conn_handle;
  h2_bloomspeaker_engine_advertising_control_fn pause_management_advertising;
  h2_bloomspeaker_engine_advertising_control_fn resume_management_advertising;
  void *management_advertising_user;
  bool pairing_active;
  bool scan_active;
  bool advertising_active;
  bool management_advertising_paused;
  bool client_started;
};

static uint64_t now_ms(h2_bloomspeaker_engine_t *engine) {
  uint64_t value = 0u;
  (void)h2_pal_time_get_monotonic_ms(engine->runtime->time, &value);
  return value;
}

static void log_stage(h2_bloomspeaker_engine_t *engine, const char *stage,
                      int result) {
  if (engine->runtime->log == NULL) {
    return;
  }
  char message[128];
  (void)snprintf(message, sizeof(message), "stage=%s rc=%d peer=%010llx",
                 stage, result, (unsigned long long)engine->peer_tag);
  (void)h2_pal_log_write(engine->runtime->log,
                         result == H2_PAL_OK ? H2_PAL_LOG_INFO
                                             : H2_PAL_LOG_ERROR,
                         "lua-bloomspeaker/ble", message);
}

static uint64_t read_le(const uint8_t *data, size_t size) {
  uint64_t value = 0u;
  for (size_t index = 0u; index < size; ++index) {
    value |= (uint64_t)data[index] << (index * 8u);
  }
  return value;
}

static uint64_t mix64(uint64_t value) {
  value ^= value >> 30u;
  value *= UINT64_C(0xbf58476d1ce4e5b9);
  value ^= value >> 27u;
  value *= UINT64_C(0x94d049bb133111eb);
  return value ^ (value >> 31u);
}

static uint32_t derive_passkey(uint64_t local_tag, uint64_t local_ticket,
                               uint64_t peer_tag, uint64_t peer_ticket) {
  uint64_t low_tag = local_tag < peer_tag ? local_tag : peer_tag;
  uint64_t high_tag = local_tag < peer_tag ? peer_tag : local_tag;
  uint64_t low_ticket = local_ticket < peer_ticket ? local_ticket : peer_ticket;
  uint64_t high_ticket = local_ticket < peer_ticket ? peer_ticket : local_ticket;
  uint64_t mixed = mix64(low_tag ^ (high_tag << 17u) ^ low_ticket ^
                         (high_ticket << 29u));
  return 100000u + (uint32_t)(mixed % 900000u);
}

static h2_bleikcp_api_t stream_api(h2_bloomspeaker_engine_t *engine) {
  return (h2_bleikcp_api_t){
      .ble = engine->runtime->ble_host,
      .task = engine->runtime->task,
      .time = engine->runtime->time,
      .sync = engine->runtime->sync,
      .system_event = engine->runtime->system_event,
      .allocator = engine->runtime->mem,
  };
}

static h2_bleikcp_config_t stream_config(h2_bloomspeaker_engine_t *engine) {
  return (h2_bleikcp_config_t){
      .service_uuid = {s_service_uuid, sizeof(s_service_uuid)},
      .tx_char_uuid = {s_tx_uuid, sizeof(s_tx_uuid)},
      .rx_char_uuid = {s_rx_uuid, sizeof(s_rx_uuid)},
      .send_window = H2_BLOOMSPEAKER_STREAM_WINDOW,
      .recv_window = H2_BLOOMSPEAKER_STREAM_WINDOW,
      .input_frame_capacity = 16u,
      .tx_buffer_size = H2_BLOOMSPEAKER_STREAM_BUFFER_SIZE,
      .rx_buffer_size = H2_BLOOMSPEAKER_STREAM_BUFFER_SIZE,
      .nodelay = 1,
      .interval_ms = 10,
      .resend = 2,
      .no_congestion_control = 1,
      .output_retry_count = 40u,
      .output_retry_delay_ms = 2u,
      .setup_timeout_ms = H2_BLOOMSPEAKER_SETUP_TIMEOUT_MS,
      .worker_task_options = {h2_bleikcp_worker_task_name, 12u * 1024u},
      .server_task_options = {h2_bleikcp_server_task_name,
                              H2_BLOOMSPEAKER_CODEC_TASK_STACK_SIZE},
      .user = engine,
  };
}

static h2_bloomspeaker_state_t current_state(
    h2_bloomspeaker_engine_t *engine) {
  h2_bloomspeaker_snapshot_t snapshot;
  h2_bloomspeaker_controller_snapshot(engine->controller, &snapshot);
  return snapshot.state;
}

static bool session_should_run(h2_bloomspeaker_engine_t *engine) {
  h2_bloomspeaker_state_t state = current_state(engine);
  return !atomic_load_explicit(&engine->stop, memory_order_acquire) &&
         (state == H2_BLOOMSPEAKER_STATE_SECURING ||
          state == H2_BLOOMSPEAKER_STATE_TALKING);
}

static bool setup_should_run(h2_bloomspeaker_engine_t *engine) {
  h2_bloomspeaker_state_t state = current_state(engine);
  return !atomic_load_explicit(&engine->stop, memory_order_acquire) &&
         (state == H2_BLOOMSPEAKER_STATE_CONNECTING ||
          state == H2_BLOOMSPEAKER_STATE_SECURING);
}

static bool audio_session_should_run(void *user) {
  return session_should_run(user);
}

static void connection_store(h2_bloomspeaker_engine_t *engine,
                             uint16_t conn_handle) {
  atomic_store_explicit(&engine->conn_handle, conn_handle,
                        memory_order_release);
}

static void connection_disconnect(h2_bloomspeaker_engine_t *engine,
                                  uint16_t conn_handle) {
  uint16_t expected = conn_handle;
  if (conn_handle != H2_PAL_BLE_INVALID_CONN_HANDLE &&
      atomic_compare_exchange_strong_explicit(
          &engine->conn_handle, &expected, H2_PAL_BLE_INVALID_CONN_HANDLE,
          memory_order_acq_rel, memory_order_acquire)) {
    (void)h2_pal_ble_disconnect(engine->runtime->ble_host, conn_handle);
  }
}

static void connection_disconnect_current(h2_bloomspeaker_engine_t *engine) {
  uint16_t conn_handle = atomic_exchange_explicit(
      &engine->conn_handle, H2_PAL_BLE_INVALID_CONN_HANDLE,
      memory_order_acq_rel);
  if (conn_handle != H2_PAL_BLE_INVALID_CONN_HANDLE) {
    (void)h2_pal_ble_disconnect(engine->runtime->ble_host, conn_handle);
  }
}

static bool address_equal(const h2_pal_ble_addr_t *left,
                          const h2_pal_ble_addr_t *right) {
  return left != NULL && right != NULL &&
         memcmp(left->value, right->value, sizeof(left->value)) == 0;
}

/*
 * The central can observe our CLAIMED beacon and connect before our next scan
 * report contains its corresponding CLAIMED/LOCKED beacon.  Treat an inbound
 * connection from the peer we already claimed as the final mutual-lock event.
 * This installs the peripheral passkey before the central starts SMP and also
 * closes the short CLAIMING -> CONNECTING race.
 */
static int accept_claimed_inbound(h2_bloomspeaker_engine_t *engine,
                                  const h2_pal_ble_connection_t *connection) {
  uint64_t peer = 0u;
  uint64_t peer_ticket = 0u;
  uint32_t peer_epoch = 0u;
  uint64_t local_tag = 0u;
  uint64_t local_ticket = 0u;
  bool peer_found = false;
  bool locked = false;
  int result = h2_pal_mutex_lock(engine->runtime->sync,
                                 engine->pairing_mutex);
  if (result != H2_PAL_OK) {
    return result;
  }
  if (engine->pairing.local.state ==
          H2_BLOOMSPEAKER_PAIRING_BEACON_CLAIMED &&
      engine->pairing.local.claim_target != 0u) {
    peer = engine->pairing.local.claim_target;
    for (size_t index = 0u;
         index < H2_BLOOMSPEAKER_PAIRING_MAX_CANDIDATES; ++index) {
      const h2_bloomspeaker_observed_address_t *address =
          &engine->addresses[index];
      const h2_bloomspeaker_pairing_candidate_t *candidate =
          &engine->pairing.candidates[index];
      if (address->occupied && address->device_tag == peer &&
          address_equal(&address->address, &connection->peer_addr)) {
        for (size_t candidate_index = 0u;
             candidate_index < H2_BLOOMSPEAKER_PAIRING_MAX_CANDIDATES;
             ++candidate_index) {
          candidate = &engine->pairing.candidates[candidate_index];
          if (candidate->occupied &&
              candidate->beacon.device_tag == peer) {
            peer_ticket = candidate->beacon.ticket;
            peer_epoch = candidate->beacon.epoch;
            peer_found = true;
            break;
          }
        }
        if (peer_found &&
            h2_bloomspeaker_pairing_role(&engine->pairing, peer) ==
                H2_BLOOMSPEAKER_PAIRING_ROLE_PERIPHERAL) {
          locked = h2_bloomspeaker_pairing_lock(&engine->pairing, peer);
          local_tag = engine->pairing.local.device_tag;
          local_ticket = engine->pairing.local.ticket;
        }
        break;
      }
    }
  }
  (void)h2_pal_mutex_unlock(engine->runtime->sync, engine->pairing_mutex);
  if (!locked) {
    return H2_PAL_ERR_NOT_FOUND;
  }

  engine->peer_tag = peer;
  engine->peer_epoch = peer_epoch;
  engine->peer_address = connection->peer_addr;
  engine->role = H2_BLOOMSPEAKER_PAIRING_ROLE_PERIPHERAL;
  engine->pairing_passkey =
      derive_passkey(local_tag, local_ticket, peer, peer_ticket);
  const h2_pal_ble_pairing_config_t security = {
      .enabled = true,
      .passkey = engine->pairing_passkey,
      .io = H2_PAL_BLE_PAIRING_IO_DISPLAY_ONLY,
  };
  result = h2_pal_ble_configure_pairing(engine->runtime->ble_host,
                                        &security);
  if (result == H2_PAL_OK &&
      !h2_bloomspeaker_controller_transition(
          engine->controller, H2_BLOOMSPEAKER_STATE_CLAIMING,
          H2_BLOOMSPEAKER_STATE_CONNECTING, now_ms(engine), peer, 0)) {
    result = H2_PAL_ERR_CLOSED;
  }
  if (result == H2_PAL_OK) {
    connection_store(engine, connection->conn_handle);
  }
  log_stage(engine, "inbound_lock", result);
  return result;
}

static void transition_active_to_error(h2_bloomspeaker_engine_t *engine,
                                       int error) {
  const h2_bloomspeaker_state_t states[] = {
      H2_BLOOMSPEAKER_STATE_PAIRING, H2_BLOOMSPEAKER_STATE_CLAIMING,
      H2_BLOOMSPEAKER_STATE_CONNECTING, H2_BLOOMSPEAKER_STATE_SECURING,
      H2_BLOOMSPEAKER_STATE_TALKING,
  };
  for (size_t index = 0u; index < sizeof(states) / sizeof(states[0]); ++index) {
    if (h2_bloomspeaker_controller_transition(
            engine->controller, states[index], H2_BLOOMSPEAKER_STATE_ERROR,
            now_ms(engine), 0u, error)) {
      return;
    }
  }
}

static int engine_system_event(void *user,
                               const h2_pal_system_event_t *event) {
  h2_bloomspeaker_engine_t *engine = user;
  if (engine == NULL || event == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (event->type == H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED &&
      event->payload_size == sizeof(h2_pal_ble_connection_t)) {
    const h2_pal_ble_connection_t *connection = event->payload;
    if (connection->role == H2_PAL_BLE_ROLE_PERIPHERAL) {
      h2_bloomspeaker_state_t state = current_state(engine);
      if (engine->role == H2_BLOOMSPEAKER_PAIRING_ROLE_PERIPHERAL &&
          state == H2_BLOOMSPEAKER_STATE_CONNECTING) {
        connection_store(engine, connection->conn_handle);
      } else if (engine->role == H2_BLOOMSPEAKER_PAIRING_ROLE_NONE &&
                 state == H2_BLOOMSPEAKER_STATE_CLAIMING &&
                 accept_claimed_inbound(engine, connection) != H2_PAL_OK) {
        (void)h2_pal_ble_disconnect(engine->runtime->ble_host,
                                    connection->conn_handle);
      }
    }
  } else if (event->type == H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED &&
             event->payload_size == sizeof(h2_pal_ble_disconnected_info_t)) {
    const h2_pal_ble_disconnected_info_t *info = event->payload;
    uint16_t expected = info->conn_handle;
    if (atomic_compare_exchange_strong_explicit(
            &engine->conn_handle, &expected,
            H2_PAL_BLE_INVALID_CONN_HANDLE, memory_order_acq_rel,
            memory_order_acquire)) {
      h2_bloomspeaker_state_t state = current_state(engine);
      if (state == H2_BLOOMSPEAKER_STATE_CONNECTING ||
          state == H2_BLOOMSPEAKER_STATE_SECURING) {
        transition_active_to_error(engine, H2_PAL_ERR_CLOSED);
      }
    }
  }
  return H2_PAL_OK;
}

static void unsubscribe_system_events(h2_bloomspeaker_engine_t *engine);

static int subscribe_system_events(h2_bloomspeaker_engine_t *engine) {
  static const h2_pal_system_event_type_t event_types[] = {
      H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED,
      H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED,
  };
  for (size_t index = 0u;
       index < H2_BLOOMSPEAKER_SYSTEM_EVENT_SUBSCRIPTION_COUNT; ++index) {
    int result = h2_pal_system_event_subscribe(
        engine->runtime->system_event, event_types[index], engine_system_event,
        engine, &engine->system_event_subscriptions[index]);
    if (result == H2_PAL_ERR_UNSUPPORTED) {
      unsubscribe_system_events(engine);
      return H2_PAL_OK;
    }
    if (result != H2_PAL_OK) {
      return result;
    }
  }
  return H2_PAL_OK;
}

static void unsubscribe_system_events(h2_bloomspeaker_engine_t *engine) {
  for (size_t index = H2_BLOOMSPEAKER_SYSTEM_EVENT_SUBSCRIPTION_COUNT;
       index > 0u; --index) {
    h2_pal_system_event_unsubscribe(
        engine->runtime->system_event,
        engine->system_event_subscriptions[index - 1u]);
    engine->system_event_subscriptions[index - 1u] = NULL;
  }
}

static void finish_normal_session(h2_bloomspeaker_engine_t *engine) {
  h2_bloomspeaker_snapshot_t snapshot;
  h2_bloomspeaker_controller_snapshot(engine->controller, &snapshot);
  if (snapshot.state == H2_BLOOMSPEAKER_STATE_TALKING) {
    (void)h2_bloomspeaker_controller_transition(
        engine->controller, H2_BLOOMSPEAKER_STATE_TALKING,
        H2_BLOOMSPEAKER_STATE_DISCONNECTING, now_ms(engine),
        engine->peer_tag, 0);
    h2_bloomspeaker_controller_snapshot(engine->controller, &snapshot);
  }
  if (snapshot.state == H2_BLOOMSPEAKER_STATE_DISCONNECTING) {
    uint64_t now = now_ms(engine);
    uint64_t elapsed = now >= snapshot.state_entered_ms
                           ? now - snapshot.state_entered_ms
                           : 0u;
    if (elapsed < H2_BLOOMSPEAKER_DISCONNECT_ANIMATION_MS &&
        !atomic_load_explicit(&engine->stop, memory_order_acquire)) {
      (void)h2_pal_time_sleep_ms(
          engine->runtime->time,
          (uint32_t)(H2_BLOOMSPEAKER_DISCONNECT_ANIMATION_MS - elapsed));
    }
  }
  if (!atomic_load_explicit(&engine->stop, memory_order_acquire)) {
    (void)h2_bloomspeaker_controller_transition(
        engine->controller, H2_BLOOMSPEAKER_STATE_DISCONNECTING,
        H2_BLOOMSPEAKER_STATE_IDLE, now_ms(engine), 0u, 0);
  }
}

static int session_wait(h2_bloomspeaker_engine_t *engine,
                        h2_bleikcp_t *stream) {
  uint8_t ignored[64];
  while (session_should_run(engine)) {
    size_t read_size = 0u;
    int result = h2_bleikcp_read(stream, ignored, sizeof(ignored), &read_size,
                                 H2_BLOOMSPEAKER_STREAM_TIMEOUT_MS);
    if (result == H2_PAL_ERR_CLOSED) {
      return H2_PAL_OK;
    }
    if (result != H2_PAL_OK && result != H2_PAL_ERR_TIMEOUT &&
        result != H2_PAL_ERR_WOULD_BLOCK) {
      return result;
    }
  }
  return H2_PAL_OK;
}

static int server_handler(void *user, h2_bleikcp_t *stream,
                          uint16_t conn_handle) {
  h2_bloomspeaker_engine_t *engine = user;
  (void)conn_handle;
  if (engine == NULL || stream == NULL ||
      engine->role != H2_BLOOMSPEAKER_PAIRING_ROLE_PERIPHERAL) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (!h2_bloomspeaker_controller_transition(
          engine->controller, H2_BLOOMSPEAKER_STATE_CONNECTING,
          H2_BLOOMSPEAKER_STATE_SECURING, now_ms(engine), engine->peer_tag,
          0)) {
    return H2_PAL_ERR_CLOSED;
  }
  uint8_t frame[H2_BLOOMSPEAKER_HANDSHAKE_SIZE];
  size_t frame_size = 0u;
  int result = h2_bleikcp_read(stream, frame, sizeof(frame), &frame_size,
                               H2_BLOOMSPEAKER_SETUP_TIMEOUT_MS);
  log_stage(engine, "server_handshake_read", result);
  if (result == H2_PAL_OK &&
      !h2_bloomspeaker_handshake_valid(
          frame, frame_size, engine->peer_tag,
          engine->pairing.local.device_tag, engine->peer_epoch)) {
    result = H2_PAL_ERR_FORMAT;
  }
  if (result == H2_PAL_OK) {
    h2_bloomspeaker_handshake_make(
        frame, engine->pairing.local.device_tag, engine->peer_tag,
        engine->pairing.local.epoch);
    result = h2_bleikcp_write(stream, frame, sizeof(frame),
                              H2_BLOOMSPEAKER_SETUP_TIMEOUT_MS);
  }
  if (result == H2_PAL_OK) {
    result = h2_bleikcp_flush(stream, H2_BLOOMSPEAKER_SETUP_TIMEOUT_MS);
    log_stage(engine, "server_handshake_flush", result);
  }
  if (result == H2_PAL_OK && !setup_should_run(engine)) {
    result = H2_PAL_ERR_CLOSED;
  }
  if (result == H2_PAL_OK &&
      !h2_bloomspeaker_controller_transition(
          engine->controller, H2_BLOOMSPEAKER_STATE_SECURING,
          H2_BLOOMSPEAKER_STATE_TALKING, now_ms(engine), engine->peer_tag,
          0)) {
    result = H2_PAL_ERR_CLOSED;
  }
  if (result == H2_PAL_OK) {
    result = engine->audio != NULL
                 ? h2_bloomspeaker_audio_run_session(
                       engine->audio, stream, audio_session_should_run, engine)
                 : session_wait(engine, stream);
  }
  if (result == H2_PAL_ERR_TIMEOUT) {
    log_stage(engine, "session_rx_timeout", result);
  }
  if (!atomic_load_explicit(&engine->stop, memory_order_acquire)) {
    h2_bloomspeaker_state_t state = current_state(engine);
    bool cancelled = result == H2_PAL_ERR_CLOSED ||
                     result == H2_PAL_ERR_TIMEOUT ||
                     state == H2_BLOOMSPEAKER_STATE_IDLE ||
                     state == H2_BLOOMSPEAKER_STATE_DISCONNECTING;
    if ((result == H2_PAL_OK || cancelled) &&
        state != H2_BLOOMSPEAKER_STATE_IDLE) {
      finish_normal_session(engine);
      result = H2_PAL_OK;
    } else if (result == H2_PAL_OK || cancelled) {
      result = H2_PAL_OK;
    } else {
      transition_active_to_error(engine, result);
    }
  }
  return result;
}

static bool scan_result(void *user, const h2_pal_ble_scan_result_t *result) {
  h2_bloomspeaker_engine_t *engine = user;
  if (engine == NULL || result == NULL ||
      !result->connectable ||
      result->data_status != H2_PAL_BLE_ADV_DATA_COMPLETE ||
      result->manufacturer_data.len != H2_BLOOMSPEAKER_PAIRING_BEACON_SIZE) {
    return false;
  }
  h2_bloomspeaker_pairing_beacon_t beacon;
  if (h2_bloomspeaker_pairing_decode(result->manufacturer_data.data,
                                     result->manufacturer_data.len,
                                     &beacon) != H2_PAL_OK) {
    return false;
  }
  if (h2_pal_mutex_lock(engine->runtime->sync, engine->pairing_mutex) !=
      H2_PAL_OK) {
    return false;
  }
  uint64_t observed_ms = now_ms(engine);
  if (h2_bloomspeaker_pairing_observe(&engine->pairing, &beacon, result->rssi,
                                       observed_ms) != H2_PAL_OK) {
    (void)h2_pal_mutex_unlock(engine->runtime->sync, engine->pairing_mutex);
    return false;
  }
  h2_bloomspeaker_observed_address_t *slot = NULL;
  h2_bloomspeaker_observed_address_t *oldest = NULL;
  for (size_t index = 0u; index < H2_BLOOMSPEAKER_PAIRING_MAX_CANDIDATES;
       ++index) {
    h2_bloomspeaker_observed_address_t *candidate = &engine->addresses[index];
    if (candidate->occupied && candidate->device_tag == beacon.device_tag) {
      slot = candidate;
      break;
    }
    if (!candidate->occupied && slot == NULL) {
      slot = candidate;
    }
    if (candidate->occupied &&
        (oldest == NULL || candidate->last_seen_ms < oldest->last_seen_ms)) {
      oldest = candidate;
    }
  }
  if (slot == NULL) {
    slot = oldest;
  }
  if (slot != NULL) {
    slot->occupied = true;
    slot->device_tag = beacon.device_tag;
    slot->last_seen_ms = observed_ms;
    slot->address = result->addr;
  }
  (void)h2_pal_mutex_unlock(engine->runtime->sync, engine->pairing_mutex);
  return false;
}

static int update_advertising(h2_bloomspeaker_engine_t *engine) {
  uint8_t beacon[H2_BLOOMSPEAKER_PAIRING_BEACON_SIZE];
  int result = h2_bloomspeaker_pairing_encode(&engine->pairing.local, beacon);
  if (result != H2_PAL_OK) {
    return result;
  }
  const h2_pal_ble_adv_data_t data = {
      .manufacturer_data = {beacon, sizeof(beacon)},
  };
  return h2_pal_ble_adv_set_set_data(engine->runtime->ble_host,
                                    engine->advertising_set, &data);
}

static int begin_pairing(h2_bloomspeaker_engine_t *engine) {
  uint8_t random[20];
  int result = h2_pal_crypto_random(engine->runtime->crypto, random,
                                    sizeof(random));
  if (result != H2_PAL_OK) {
    return result;
  }
  uint64_t tag = read_le(random, 5u) & H2_BLOOMSPEAKER_PAIRING_DEVICE_TAG_MASK;
  uint64_t ticket = read_le(random + 5u, 8u);
  uint32_t epoch = (uint32_t)read_le(random + 13u, 4u);
  if (tag == 0u) {
    tag = 1u;
  }
  memset(engine->addresses, 0, sizeof(engine->addresses));
  h2_bloomspeaker_pairing_init(&engine->pairing, tag, ticket, epoch);

  if (engine->pause_management_advertising != NULL) {
    result = engine->pause_management_advertising(
        engine->management_advertising_user);
    if (result != H2_PAL_OK) {
      return result;
    }
    engine->management_advertising_paused = true;
  }

  h2_bleikcp_api_t api = stream_api(engine);
  h2_bleikcp_config_t config = stream_config(engine);
  result = h2_bleikcp_server_open(&api, &config, server_handler, engine,
                                  &engine->server);
  if (result != H2_PAL_OK) {
    return result;
  }
  const h2_pal_ble_adv_params_t adv_params = {
      .mode = H2_PAL_BLE_ADV_MODE_CONNECTABLE,
      .interval_min_ms = 60u,
      .interval_max_ms = 80u,
      .type = H2_PAL_BLE_ADV_TYPE_LEGACY,
      .primary_phy = H2_PAL_BLE_PHY_1M,
  };
  result = h2_pal_ble_adv_set_create(engine->runtime->ble_host, &adv_params,
                                     &engine->advertising_set);
  if (result == H2_PAL_OK) {
    result = update_advertising(engine);
  }
  if (result == H2_PAL_OK) {
    result = h2_pal_ble_adv_set_start(engine->runtime->ble_host,
                                      engine->advertising_set);
    engine->advertising_active = result == H2_PAL_OK;
  }
  const h2_pal_ble_scan_params_t scan_params = {
      /* ESP places legacy manufacturer payloads in scan response data. */
      .mode = H2_PAL_BLE_SCAN_MODE_ACTIVE,
      .interval_ms = 60u,
      .window_ms = 45u,
      .timeout_ms = 0u,
      .type = H2_PAL_BLE_SCAN_TYPE_LEGACY,
      .phy_mask = H2_PAL_BLE_SCAN_PHY_1M,
  };
  if (result == H2_PAL_OK) {
    result = h2_pal_ble_start_scan(engine->runtime->ble_host, &scan_params,
                                   scan_result, engine);
    engine->scan_active = result == H2_PAL_OK;
  }
  engine->pairing_active = result == H2_PAL_OK;
  return result;
}

static void cleanup_pairing(h2_bloomspeaker_engine_t *engine) {
  if (engine->scan_active) {
    (void)h2_pal_ble_stop_scan(engine->runtime->ble_host);
    engine->scan_active = false;
  }
  connection_disconnect_current(engine);
  if (engine->advertising_set != NULL) {
    if (engine->advertising_active) {
      (void)h2_pal_ble_adv_set_stop(engine->runtime->ble_host,
                                    engine->advertising_set);
    }
    (void)h2_pal_ble_adv_set_destroy(engine->runtime->ble_host,
                                     engine->advertising_set);
    engine->advertising_set = NULL;
    engine->advertising_active = false;
  }
  if (engine->server != NULL) {
    (void)h2_bleikcp_server_close(engine->server);
    engine->server = NULL;
  }
  const h2_pal_ble_pairing_config_t disabled = {0};
  (void)h2_pal_ble_configure_pairing(engine->runtime->ble_host, &disabled);
  engine->pairing_active = false;
  engine->client_started = false;
  engine->role = H2_BLOOMSPEAKER_PAIRING_ROLE_NONE;
  engine->peer_tag = 0u;
  engine->peer_epoch = 0u;
  if (engine->management_advertising_paused) {
    int result = H2_PAL_ERR_WOULD_BLOCK;
    for (uint32_t attempt = 0u; attempt < 12u; ++attempt) {
      result = engine->resume_management_advertising(
          engine->management_advertising_user);
      if (result == H2_PAL_OK) {
        engine->management_advertising_paused = false;
        break;
      }
      if (result != H2_PAL_ERR_NO_MEMORY &&
          result != H2_PAL_ERR_WOULD_BLOCK && result != H2_PAL_ERR_BUSY) {
        break;
      }
      (void)h2_pal_time_sleep_ms(engine->runtime->time, 50u);
    }
    log_stage(engine, "management_resume", result);
  }
}

static bool copy_peer_address(h2_bloomspeaker_engine_t *engine,
                              uint64_t peer,
                              h2_pal_ble_addr_t *out_address) {
  for (size_t index = 0u; index < H2_BLOOMSPEAKER_PAIRING_MAX_CANDIDATES;
       ++index) {
    const h2_bloomspeaker_observed_address_t *candidate =
        &engine->addresses[index];
    if (candidate->occupied && candidate->device_tag == peer) {
      *out_address = candidate->address;
      return true;
    }
  }
  return false;
}

static int lock_pair(h2_bloomspeaker_engine_t *engine, uint64_t peer,
                     uint64_t now) {
  uint64_t peer_ticket = 0u;
  uint32_t peer_epoch = 0u;
  uint64_t local_tag = 0u;
  uint64_t local_ticket = 0u;
  h2_pal_ble_addr_t address;
  h2_bloomspeaker_pairing_role_t role = H2_BLOOMSPEAKER_PAIRING_ROLE_NONE;
  if (h2_pal_mutex_lock(engine->runtime->sync, engine->pairing_mutex) !=
      H2_PAL_OK) {
    return H2_PAL_ERR_BUSY;
  }
  bool valid = h2_bloomspeaker_pairing_lock(&engine->pairing, peer) &&
               copy_peer_address(engine, peer, &address);
  if (valid) {
    role = h2_bloomspeaker_pairing_role(&engine->pairing, peer);
    for (size_t index = 0u;
         index < H2_BLOOMSPEAKER_PAIRING_MAX_CANDIDATES; ++index) {
      if (engine->pairing.candidates[index].occupied &&
          engine->pairing.candidates[index].beacon.device_tag == peer) {
        peer_ticket = engine->pairing.candidates[index].beacon.ticket;
        peer_epoch = engine->pairing.candidates[index].beacon.epoch;
        break;
      }
    }
    local_tag = engine->pairing.local.device_tag;
    local_ticket = engine->pairing.local.ticket;
  }
  (void)h2_pal_mutex_unlock(engine->runtime->sync, engine->pairing_mutex);
  if (!valid || role == H2_BLOOMSPEAKER_PAIRING_ROLE_NONE) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  engine->peer_tag = peer;
  engine->peer_epoch = peer_epoch;
  engine->peer_address = address;
  engine->role = role;
  engine->pairing_passkey =
      derive_passkey(local_tag, local_ticket, peer, peer_ticket);
  int result = update_advertising(engine);
  if (engine->scan_active) {
    int stop_result = h2_pal_ble_stop_scan(engine->runtime->ble_host);
    if (result == H2_PAL_OK && stop_result != H2_PAL_OK &&
        stop_result != H2_PAL_ERR_INVALID_STATE) {
      result = stop_result;
    }
    engine->scan_active = false;
  }
  const h2_pal_ble_pairing_config_t security = {
      .enabled = true,
      .passkey = engine->pairing_passkey,
      .io = role == H2_BLOOMSPEAKER_PAIRING_ROLE_PERIPHERAL
                ? H2_PAL_BLE_PAIRING_IO_DISPLAY_ONLY
                : H2_PAL_BLE_PAIRING_IO_KEYBOARD_ONLY,
  };
  if (result == H2_PAL_OK) {
    result = h2_pal_ble_configure_pairing(engine->runtime->ble_host,
                                          &security);
  }
  if (result == H2_PAL_OK &&
      !h2_bloomspeaker_controller_transition(
          engine->controller, H2_BLOOMSPEAKER_STATE_CLAIMING,
          H2_BLOOMSPEAKER_STATE_CONNECTING, now, peer, 0)) {
    result = H2_PAL_ERR_CLOSED;
  }
  return result;
}

static int progress_claim(h2_bloomspeaker_engine_t *engine, uint64_t now) {
  h2_bloomspeaker_snapshot_t snapshot;
  h2_bloomspeaker_controller_snapshot(engine->controller, &snapshot);
  h2_bloomspeaker_state_t state = snapshot.state;
  uint64_t selected = 0u;
  uint64_t mutual = 0u;
  if (h2_pal_mutex_lock(engine->runtime->sync, engine->pairing_mutex) !=
      H2_PAL_OK) {
    return H2_PAL_ERR_BUSY;
  }
  if (state == H2_BLOOMSPEAKER_STATE_PAIRING) {
    selected = h2_bloomspeaker_pairing_select(&engine->pairing, now);
    if (selected != 0u) {
      h2_bloomspeaker_pairing_set_claim(&engine->pairing, selected);
    }
  } else if (state == H2_BLOOMSPEAKER_STATE_CLAIMING &&
             now >= snapshot.state_entered_ms &&
             now - snapshot.state_entered_ms >=
                 H2_BLOOMSPEAKER_CLAIM_TIMEOUT_MS) {
    h2_bloomspeaker_pairing_set_claim(&engine->pairing, 0u);
    selected = UINT64_MAX;
  } else if (state == H2_BLOOMSPEAKER_STATE_CLAIMING) {
    (void)h2_bloomspeaker_pairing_mutual_claim(&engine->pairing, now,
                                                &mutual);
  }
  (void)h2_pal_mutex_unlock(engine->runtime->sync, engine->pairing_mutex);
  if (selected == UINT64_MAX) {
    int result = update_advertising(engine);
    if (result == H2_PAL_OK) {
      (void)h2_bloomspeaker_controller_transition(
          engine->controller, H2_BLOOMSPEAKER_STATE_CLAIMING,
          H2_BLOOMSPEAKER_STATE_PAIRING, now, 0u, 0);
    }
    return result;
  }
  if (selected != 0u) {
    int result = update_advertising(engine);
    if (result == H2_PAL_OK) {
      (void)h2_bloomspeaker_controller_transition(
          engine->controller, H2_BLOOMSPEAKER_STATE_PAIRING,
          H2_BLOOMSPEAKER_STATE_CLAIMING, now, selected, 0);
    }
    return result;
  }
  return mutual != 0u ? lock_pair(engine, mutual, now) : H2_PAL_OK;
}

static int run_client(h2_bloomspeaker_engine_t *engine) {
  engine->client_started = true;
  if (engine->advertising_active) {
    (void)h2_pal_ble_adv_set_stop(engine->runtime->ble_host,
                                  engine->advertising_set);
    engine->advertising_active = false;
  }
  const h2_pal_ble_connect_params_t params = {
      .timeout_ms = H2_BLOOMSPEAKER_SETUP_TIMEOUT_MS,
      .interval_min_ms = 15u,
      .interval_max_ms = 15u,
      .latency = 0u,
      .supervision_timeout_ms = 4000u,
  };
  uint16_t conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
  int result = h2_pal_ble_connect(engine->runtime->ble_host,
                                  &engine->peer_address, &params,
                                  &conn_handle);
  log_stage(engine, "client_connect", result);
  if (result == H2_PAL_OK) {
    connection_store(engine, conn_handle);
  }
  if (result == H2_PAL_OK && !setup_should_run(engine)) {
    result = H2_PAL_ERR_CLOSED;
  }
  if (result == H2_PAL_OK &&
      !h2_bloomspeaker_controller_transition(
          engine->controller, H2_BLOOMSPEAKER_STATE_CONNECTING,
          H2_BLOOMSPEAKER_STATE_SECURING, now_ms(engine), engine->peer_tag,
          0)) {
    result = H2_PAL_ERR_CLOSED;
  }
  if (result == H2_PAL_OK) {
    result = h2_pal_ble_pair(engine->runtime->ble_host, conn_handle,
                             H2_BLOOMSPEAKER_PAIR_TIMEOUT_MS);
    log_stage(engine, "client_pair", result);
  }
  if (result == H2_PAL_OK && !setup_should_run(engine)) {
    result = H2_PAL_ERR_CLOSED;
  }
  uint16_t mtu = 0u;
  if (result == H2_PAL_OK) {
    result = h2_pal_ble_exchange_mtu(engine->runtime->ble_host,
                                     conn_handle, &mtu,
                                     H2_BLOOMSPEAKER_SETUP_TIMEOUT_MS);
    log_stage(engine, "client_mtu", result);
  }
  if (result == H2_PAL_OK && !setup_should_run(engine)) {
    result = H2_PAL_ERR_CLOSED;
  }
  if (result == H2_PAL_OK) {
    (void)h2_pal_ble_set_preferred_phy(
        engine->runtime->ble_host, conn_handle, H2_PAL_BLE_PHY_2M,
        H2_PAL_BLE_PHY_2M, H2_BLOOMSPEAKER_SETUP_TIMEOUT_MS);
  }
  h2_bleikcp_t *stream = NULL;
  h2_bleikcp_api_t api = stream_api(engine);
  h2_bleikcp_config_t config = stream_config(engine);
  if (result == H2_PAL_OK) {
    result = h2_bleikcp_client_open(&api, &config, conn_handle, mtu,
                                    &stream);
    log_stage(engine, "client_stream", result);
  }
  if (result == H2_PAL_OK && !setup_should_run(engine)) {
    result = H2_PAL_ERR_CLOSED;
  }
  uint8_t frame[H2_BLOOMSPEAKER_HANDSHAKE_SIZE];
  if (result == H2_PAL_OK) {
    h2_bloomspeaker_handshake_make(
        frame, engine->pairing.local.device_tag, engine->peer_tag,
        engine->pairing.local.epoch);
    result = h2_bleikcp_write(stream, frame, sizeof(frame),
                              H2_BLOOMSPEAKER_SETUP_TIMEOUT_MS);
  }
  if (result == H2_PAL_OK) {
    result = h2_bleikcp_flush(stream, H2_BLOOMSPEAKER_SETUP_TIMEOUT_MS);
    log_stage(engine, "client_handshake_flush", result);
  }
  size_t frame_size = 0u;
  if (result == H2_PAL_OK) {
    result = h2_bleikcp_read(stream, frame, sizeof(frame), &frame_size,
                             H2_BLOOMSPEAKER_SETUP_TIMEOUT_MS);
    log_stage(engine, "client_handshake_read", result);
  }
  if (result == H2_PAL_OK &&
      !h2_bloomspeaker_handshake_valid(
          frame, frame_size, engine->peer_tag,
          engine->pairing.local.device_tag, engine->peer_epoch)) {
    result = H2_PAL_ERR_FORMAT;
  }
  if (result == H2_PAL_OK && !setup_should_run(engine)) {
    result = H2_PAL_ERR_CLOSED;
  }
  if (result == H2_PAL_OK &&
      !h2_bloomspeaker_controller_transition(
          engine->controller, H2_BLOOMSPEAKER_STATE_SECURING,
          H2_BLOOMSPEAKER_STATE_TALKING, now_ms(engine), engine->peer_tag,
          0)) {
    result = H2_PAL_ERR_CLOSED;
  }
  if (result == H2_PAL_OK) {
    result = engine->audio != NULL
                 ? h2_bloomspeaker_audio_run_session(
                       engine->audio, stream, audio_session_should_run, engine)
                 : session_wait(engine, stream);
  }
  if (result == H2_PAL_ERR_TIMEOUT) {
    log_stage(engine, "session_rx_timeout", result);
  }
  if (stream != NULL) {
    (void)h2_bleikcp_close(stream);
  }
  connection_disconnect(engine, conn_handle);
  h2_bloomspeaker_state_t final_state = current_state(engine);
  if (result == H2_PAL_OK || result == H2_PAL_ERR_CLOSED ||
      result == H2_PAL_ERR_TIMEOUT) {
    if (final_state != H2_BLOOMSPEAKER_STATE_IDLE) {
      finish_normal_session(engine);
    }
    result = H2_PAL_OK;
  } else {
    transition_active_to_error(engine, result);
  }
  return result;
}

static void engine_task(void *context) {
  h2_bloomspeaker_engine_t *engine = context;
  int result = h2_pal_ble_start(engine->runtime->ble_host);
  if (result != H2_PAL_OK && result != H2_PAL_ERR_INVALID_STATE) {
    h2_bloomspeaker_controller_set_state(
        engine->controller, H2_BLOOMSPEAKER_STATE_ERROR, now_ms(engine), 0u,
        result);
    return;
  }
  while (!atomic_load_explicit(&engine->stop, memory_order_acquire)) {
    h2_bloomspeaker_state_t state = current_state(engine);
    if (state == H2_BLOOMSPEAKER_STATE_PAIRING && !engine->pairing_active) {
      result = begin_pairing(engine);
      log_stage(engine, "pairing", result);
      if (result != H2_PAL_OK) {
        cleanup_pairing(engine);
        transition_active_to_error(engine, result);
      }
    } else if (engine->pairing_active &&
               (state == H2_BLOOMSPEAKER_STATE_PAIRING ||
                state == H2_BLOOMSPEAKER_STATE_CLAIMING)) {
      result = progress_claim(engine, now_ms(engine));
      if (result != H2_PAL_OK) {
        log_stage(engine, "claim", result);
        transition_active_to_error(engine, result);
      }
    } else if (engine->pairing_active &&
               state == H2_BLOOMSPEAKER_STATE_CONNECTING &&
               engine->role == H2_BLOOMSPEAKER_PAIRING_ROLE_CENTRAL &&
               !engine->client_started) {
      result = run_client(engine);
      log_stage(engine, "session", result);
    } else if (engine->pairing_active &&
               (state == H2_BLOOMSPEAKER_STATE_IDLE ||
                state == H2_BLOOMSPEAKER_STATE_ERROR)) {
      cleanup_pairing(engine);
    }
    (void)h2_pal_time_sleep_ms(engine->runtime->time,
                               H2_BLOOMSPEAKER_BLE_POLL_MS);
  }
  cleanup_pairing(engine);
}

int h2_bloomspeaker_engine_start(h2_runtime_t *runtime,
                                 h2_bloomspeaker_controller_t *controller,
                                 const h2_bloomspeaker_engine_config_t *config,
                                 h2_bloomspeaker_engine_t **out_engine) {
  if (runtime == NULL || controller == NULL || config == NULL ||
      out_engine == NULL ||
      ((config->pause_management_advertising == NULL) !=
       (config->resume_management_advertising == NULL))) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_engine = NULL;
  if (runtime->ble_host == NULL) {
    return H2_PAL_OK;
  }
  if (runtime->task == NULL || runtime->time == NULL || runtime->sync == NULL ||
      runtime->system_event == NULL || runtime->mem == NULL ||
      runtime->crypto == NULL) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  h2_bloomspeaker_engine_t *engine =
      h2_pal_mem_alloc(runtime->mem, sizeof(*engine));
  if (engine == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  memset(engine, 0, sizeof(*engine));
  engine->runtime = runtime;
  engine->controller = controller;
  engine->pause_management_advertising =
      config->pause_management_advertising;
  engine->resume_management_advertising =
      config->resume_management_advertising;
  engine->management_advertising_user = config->management_advertising_user;
  atomic_init(&engine->stop, 0);
  atomic_init(&engine->conn_handle, H2_PAL_BLE_INVALID_CONN_HANDLE);
  const h2_pal_mutex_config_t mutex_config = {
      .name = "lua-bloomspeaker/pairing",
      .allocator = runtime->mem,
      .flags = H2_PAL_MUTEX_FLAG_NONE,
  };
  int result = h2_pal_mutex_create(runtime->sync, &mutex_config,
                                   &engine->pairing_mutex);
  const h2_pal_task_options_t task_options = {
      .name = h2_bloomspeaker_ble_task_name,
      .min_stack_size = H2_BLOOMSPEAKER_CODEC_TASK_STACK_SIZE,
  };
  if (result == H2_PAL_OK) {
    result = h2_bloomspeaker_audio_start(runtime, controller, &engine->audio);
  }
  if (result == H2_PAL_OK) {
    result = subscribe_system_events(engine);
  }
  if (result == H2_PAL_OK) {
    result = h2_pal_task_start(runtime->task, &task_options, engine_task, engine,
                               &engine->task);
  }
  if (result != H2_PAL_OK) {
    unsubscribe_system_events(engine);
    if (engine->pairing_mutex != NULL) {
      (void)h2_pal_mutex_destroy(runtime->sync, engine->pairing_mutex);
    }
    (void)h2_bloomspeaker_audio_stop(engine->audio);
    h2_pal_mem_free(runtime->mem, engine);
    return result;
  }
  *out_engine = engine;
  return H2_PAL_OK;
}

int h2_bloomspeaker_engine_stop(h2_bloomspeaker_engine_t *engine) {
  if (engine == NULL) {
    return H2_PAL_OK;
  }
  atomic_store_explicit(&engine->stop, 1, memory_order_release);
  int result = h2_pal_task_join(engine->runtime->task, engine->task);
  if (result != H2_PAL_OK) {
    return result;
  }
  unsubscribe_system_events(engine);
  int audio_result = h2_bloomspeaker_audio_stop(engine->audio);
  (void)h2_pal_mutex_destroy(engine->runtime->sync, engine->pairing_mutex);
  h2_pal_mem_free(engine->runtime->mem, engine);
  return audio_result;
}
