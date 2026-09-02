#include "h2/pal/os/h2_pal_log.h"
#include "h2/pal/os/h2_pal_sync.h"
#include "h2_gizclaw_e2e_internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define H2_GIZCLAW_E2E_CONNECT_TIMEOUT_MS 30000
#define H2_GIZCLAW_E2E_WRITE_TIMEOUT_MS 15000
#define H2_GIZCLAW_E2E_OBSERVER_PEER_CAPACITY 16u
#define H2_GIZCLAW_E2E_OBSERVER_CHANNEL_CAPACITY 16u
#define H2_GIZCLAW_E2E_OBSERVER_STREAM_CAPACITY 3u
#define H2_GIZCLAW_E2E_RPC_SERVICE_LABEL "giznet/v1/service/0"

typedef struct observed_peer {
  h2_pal_webrtc_peer_t *peer;
  bool in_use;
} observed_peer_t;

typedef struct observed_channel {
  h2_pal_webrtc_channel_t *channel;
  uint16_t stream_id;
  bool open;
} observed_channel_t;

typedef struct webrtc_observer {
  const h2_pal_webrtc_api_t *upstream;
  const h2_pal_sync_api_t *sync;
  h2_pal_webrtc_api_t api;
  h2_pal_webrtc_vtable_t vtable;
  h2_pal_mutex_t *mutex;
  observed_peer_t peers[H2_GIZCLAW_E2E_OBSERVER_PEER_CAPACITY];
  observed_channel_t channels[H2_GIZCLAW_E2E_OBSERVER_CHANNEL_CAPACITY];
  uint16_t stream_ids[H2_GIZCLAW_E2E_OBSERVER_STREAM_CAPACITY];
  size_t stream_id_count;
  size_t open_channels;
  size_t max_open_channels;
  bool invalid;
  bool initialized;
} webrtc_observer_t;

static webrtc_observer_t s_webrtc_observer;

static bool observed_rpc_service(const h2_pal_webrtc_channel_info_t *info) {
  static const char label[] = H2_GIZCLAW_E2E_RPC_SERVICE_LABEL;
  return info != NULL && info->label.data != NULL &&
         info->label.len == sizeof(label) - 1u &&
         memcmp(info->label.data, label, sizeof(label) - 1u) == 0;
}

static void observe_rpc_channel(h2_pal_webrtc_channel_t *channel,
                                const h2_pal_webrtc_channel_info_t *info,
                                h2_pal_webrtc_channel_state_t state) {
  if (!s_webrtc_observer.initialized || channel == NULL ||
      !observed_rpc_service(info)) {
    return;
  }
  (void)h2_pal_mutex_lock(s_webrtc_observer.sync, s_webrtc_observer.mutex);
  observed_channel_t *observed = NULL;
  for (size_t index = 0u; index < H2_GIZCLAW_E2E_OBSERVER_CHANNEL_CAPACITY;
       ++index) {
    if (s_webrtc_observer.channels[index].channel == channel) {
      observed = &s_webrtc_observer.channels[index];
      break;
    }
    if (observed == NULL && s_webrtc_observer.channels[index].channel == NULL) {
      observed = &s_webrtc_observer.channels[index];
    }
  }
  if (observed == NULL) {
    s_webrtc_observer.invalid = true;
  } else if (state == H2_PAL_WEBRTC_CHANNEL_OPEN) {
    if (!info->has_stream_id) {
      s_webrtc_observer.invalid = true;
    }
    if (observed->channel == NULL)
      observed->channel = channel;
    if (observed->open) {
      s_webrtc_observer.invalid = true;
    } else {
      observed->open = true;
      observed->stream_id = info->stream_id;
      s_webrtc_observer.open_channels++;
      if (s_webrtc_observer.open_channels >
          s_webrtc_observer.max_open_channels) {
        s_webrtc_observer.max_open_channels = s_webrtc_observer.open_channels;
      }
      bool known = false;
      for (size_t index = 0u; index < s_webrtc_observer.stream_id_count;
           ++index) {
        known = known || s_webrtc_observer.stream_ids[index] == info->stream_id;
      }
      if (known || s_webrtc_observer.stream_id_count ==
                       H2_GIZCLAW_E2E_OBSERVER_STREAM_CAPACITY) {
        s_webrtc_observer.invalid = true;
      } else {
        s_webrtc_observer.stream_ids[s_webrtc_observer.stream_id_count++] =
            info->stream_id;
      }
    }
  } else if (state == H2_PAL_WEBRTC_CHANNEL_CLOSED ||
             state == H2_PAL_WEBRTC_CHANNEL_ERROR) {
    if (observed->channel == channel && observed->open) {
      observed->open = false;
      if (s_webrtc_observer.open_channels == 0u) {
        s_webrtc_observer.invalid = true;
      } else {
        s_webrtc_observer.open_channels--;
      }
    }
  }
  (void)h2_pal_mutex_unlock(s_webrtc_observer.sync, s_webrtc_observer.mutex);
}

static h2_pal_result_t observer_peer_create(void *user,
    h2_pal_webrtc_peer_t **out_peer) {
  webrtc_observer_t *observer = user;
  if (observer == NULL || out_peer == NULL || observer->upstream == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  observed_peer_t *slot = NULL;
  (void)h2_pal_mutex_lock(observer->sync, observer->mutex);
  for (size_t index = 0u; index < H2_GIZCLAW_E2E_OBSERVER_PEER_CAPACITY;
       ++index) {
    if (!observer->peers[index].in_use) {
      slot = &observer->peers[index];
      slot->in_use = true;
      break;
    }
  }
  (void)h2_pal_mutex_unlock(observer->sync, observer->mutex);
  if (slot == NULL)
    return H2_PAL_ERR_NO_SPACE;
  const int result = h2_pal_webrtc_peer_create(observer->upstream, out_peer);
  (void)h2_pal_mutex_lock(observer->sync, observer->mutex);
  if (result == H2_PAL_OK) {
    slot->peer = *out_peer;
  } else {
    memset(slot, 0, sizeof(*slot));
  }
  (void)h2_pal_mutex_unlock(observer->sync, observer->mutex);
  return result;
}

static h2_pal_result_t
observer_peer_add_ice_server(h2_pal_webrtc_peer_t *peer,
                             const h2_pal_webrtc_ice_server_t *server) {
  return h2_pal_webrtc_peer_add_ice_server(s_webrtc_observer.upstream, peer,
                                           server);
}

static h2_pal_result_t observer_peer_start_offer(h2_pal_webrtc_peer_t *peer) {
  return h2_pal_webrtc_peer_start_offer(s_webrtc_observer.upstream, peer);
}

static h2_pal_result_t
observer_peer_set_remote_sdp(h2_pal_webrtc_peer_t *peer,
                             h2_pal_webrtc_sdp_type_t type,
    h2_pal_webrtc_str_t sdp) {
  return h2_pal_webrtc_peer_set_remote_sdp(s_webrtc_observer.upstream, peer,
                                           type, sdp);
}

static h2_pal_result_t
observer_peer_create_data_channel(h2_pal_webrtc_peer_t *peer,
                                  const h2_pal_webrtc_channel_config_t *config,
                                  h2_pal_webrtc_channel_t **out_channel) {
  return h2_pal_webrtc_peer_create_data_channel(s_webrtc_observer.upstream,
                                                peer, config, out_channel);
}

static h2_pal_result_t observer_peer_poll(h2_pal_webrtc_peer_t *peer,
                                          int timeout_ms,
                                          h2_pal_webrtc_event_t *out_event) {
  h2_pal_result_t result = h2_pal_webrtc_peer_poll(s_webrtc_observer.upstream,
                                                   peer, timeout_ms, out_event);
  if (result == H2_PAL_OK && out_event != NULL &&
      out_event->kind == H2_PAL_WEBRTC_EVENT_CHANNEL_STATE)
    observe_rpc_channel(out_event->channel, &out_event->channel_info,
                        out_event->channel_state);
  return result;
}

static h2_pal_result_t observer_peer_set_track(h2_pal_webrtc_peer_t *peer,
                                               h2_pal_webrtc_track_t *track) {
  return h2_pal_webrtc_peer_set_track(s_webrtc_observer.upstream, peer, track);
}

static h2_pal_result_t observer_peer_unset_track(h2_pal_webrtc_peer_t *peer,
                                                 h2_pal_webrtc_track_t *track) {
  return h2_pal_webrtc_peer_unset_track(s_webrtc_observer.upstream, peer,
                                        track);
}

static h2_pal_result_t observer_peer_send_opus(h2_pal_webrtc_peer_t *peer,
                                               const uint8_t *opus,
                                               size_t opus_len) {
  return h2_pal_webrtc_peer_send_opus(s_webrtc_observer.upstream, peer, opus,
                                      opus_len);
}

static h2_pal_result_t observer_channel_send(h2_pal_webrtc_channel_t *channel,
                                             const uint8_t *data, size_t len,
    int is_text) {
  return h2_pal_webrtc_channel_send(s_webrtc_observer.upstream, channel, data,
                                    len, is_text);
}

static void observer_channel_close(h2_pal_webrtc_channel_t *channel) {
  h2_pal_webrtc_channel_close(s_webrtc_observer.upstream, channel);
}

static void observer_peer_close(h2_pal_webrtc_peer_t *peer) {
  observed_peer_t *slot = NULL;
  (void)h2_pal_mutex_lock(s_webrtc_observer.sync, s_webrtc_observer.mutex);
  for (size_t index = 0u; index < H2_GIZCLAW_E2E_OBSERVER_PEER_CAPACITY;
       ++index) {
    observed_peer_t *observed = &s_webrtc_observer.peers[index];
    if (observed->peer == peer) {
      slot = observed;
      break;
    }
  }
  (void)h2_pal_mutex_unlock(s_webrtc_observer.sync, s_webrtc_observer.mutex);
  h2_pal_webrtc_peer_close(s_webrtc_observer.upstream, peer);
  if (slot != NULL) {
    (void)h2_pal_mutex_lock(s_webrtc_observer.sync, s_webrtc_observer.mutex);
    if (slot->peer == peer)
      memset(slot, 0, sizeof(*slot));
    (void)h2_pal_mutex_unlock(s_webrtc_observer.sync, s_webrtc_observer.mutex);
  }
}

static int observer_init(const h2_pal_webrtc_api_t *upstream,
                         const h2_pal_sync_api_t *sync,
                         const h2_pal_mem_api_t *allocator) {
  if (upstream == NULL || sync == NULL || allocator == NULL ||
      s_webrtc_observer.initialized) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  memset(&s_webrtc_observer, 0, sizeof(s_webrtc_observer));
  const h2_pal_mutex_config_t mutex_config = {
      .name = "gizclaw-e2e-webrtc",
      .allocator = allocator,
      .flags = H2_PAL_MUTEX_FLAG_NONE,
  };
  int rc = h2_pal_mutex_create(sync, &mutex_config, &s_webrtc_observer.mutex);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  s_webrtc_observer.upstream = upstream;
  s_webrtc_observer.sync = sync;
  s_webrtc_observer.vtable = (h2_pal_webrtc_vtable_t){
      .peer_create = observer_peer_create,
      .peer_add_ice_server = observer_peer_add_ice_server,
      .peer_start_offer = observer_peer_start_offer,
      .peer_set_remote_sdp = observer_peer_set_remote_sdp,
      .peer_create_data_channel = observer_peer_create_data_channel,
      .peer_set_track = observer_peer_set_track,
      .peer_unset_track = observer_peer_unset_track,
      .peer_poll = observer_peer_poll,
      .peer_send_opus = observer_peer_send_opus,
      .channel_send = observer_channel_send,
      .channel_close = observer_channel_close,
      .peer_close = observer_peer_close,
  };
  s_webrtc_observer.api = (h2_pal_webrtc_api_t){
      .user = &s_webrtc_observer,
      .vtable = &s_webrtc_observer.vtable,
  };
  s_webrtc_observer.initialized = true;
  return H2_PAL_OK;
}

static void observer_deinit(void) {
  if (!s_webrtc_observer.initialized)
    return;
  s_webrtc_observer.initialized = false;
  if (s_webrtc_observer.mutex != NULL) {
    (void)h2_pal_mutex_destroy(s_webrtc_observer.sync, s_webrtc_observer.mutex);
  }
  memset(&s_webrtc_observer, 0, sizeof(s_webrtc_observer));
}

void h2_gizclaw_e2e_fixture_reset_rpc_channel_observation(void) {
  if (!s_webrtc_observer.initialized)
    return;
  (void)h2_pal_mutex_lock(s_webrtc_observer.sync, s_webrtc_observer.mutex);
  memset(s_webrtc_observer.channels, 0, sizeof(s_webrtc_observer.channels));
  memset(s_webrtc_observer.stream_ids, 0, sizeof(s_webrtc_observer.stream_ids));
  s_webrtc_observer.stream_id_count = 0u;
  s_webrtc_observer.open_channels = 0u;
  s_webrtc_observer.max_open_channels = 0u;
  s_webrtc_observer.invalid = false;
  (void)h2_pal_mutex_unlock(s_webrtc_observer.sync, s_webrtc_observer.mutex);
}

int h2_gizclaw_e2e_fixture_rpc_channel_observation(
    size_t *out_max_open, size_t *out_unique_stream_ids,
    size_t *out_open_channels) {
  if (!s_webrtc_observer.initialized || out_max_open == NULL ||
      out_unique_stream_ids == NULL || out_open_channels == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  (void)h2_pal_mutex_lock(s_webrtc_observer.sync, s_webrtc_observer.mutex);
  *out_max_open = s_webrtc_observer.max_open_channels;
  *out_unique_stream_ids = s_webrtc_observer.stream_id_count;
  *out_open_channels = s_webrtc_observer.open_channels;
  const int result =
      s_webrtc_observer.invalid ? H2_PAL_ERR_INVALID_STATE : H2_PAL_OK;
  (void)h2_pal_mutex_unlock(s_webrtc_observer.sync, s_webrtc_observer.mutex);
  return result;
}

static const uint8_t s_client_info_response[] = {
    0x0a, 0x16, 0x0a, 0x03, 'e',  '2', 'e', 0x12, 0x06, 'h', '2', 'v',
    'i',  'v',  'i',  0x1a, 0x07, 'd', 'e', 's',  'k',  't', 'o', 'p',
};

static const uint8_t s_client_identifiers_response[] = {
    0x0a, 0x0c, 0x0a, 0x0a, 'h', '2', '-', 'p', 'a', 'l', '-', 'e', '2', 'e',
};

static int key_to_base58(const uint8_t *bytes, size_t len, char *out,
                         size_t out_cap) {
  static const char alphabet[] =
      "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
  uint8_t digits[64] = {0};
  size_t leading_zeros = 0u;
  while (leading_zeros < len && bytes[leading_zeros] == 0u) {
    leading_zeros++;
  }
  size_t digit_len = 0u;
  for (size_t index = leading_zeros; index < len; ++index) {
    unsigned carry = bytes[index];
    for (size_t digit = 0u; digit < digit_len; ++digit) {
      unsigned value = (unsigned)digits[digit] * 256u + carry;
      digits[digit] = (uint8_t)(value % 58u);
      carry = value / 58u;
    }
    while (carry != 0u) {
      if (digit_len == sizeof(digits)) {
        return H2_PAL_ERR_NO_SPACE;
      }
      digits[digit_len++] = (uint8_t)(carry % 58u);
      carry /= 58u;
    }
  }
  const size_t encoded_len = leading_zeros + digit_len;
  if (encoded_len == 0u || encoded_len >= out_cap) {
    return H2_PAL_ERR_NO_SPACE;
  }
  size_t written = 0u;
  while (written < leading_zeros) {
    out[written++] = alphabet[0];
  }
  while (digit_len != 0u) {
    out[written++] = alphabet[digits[--digit_len]];
  }
  out[written] = '\0';
  return H2_PAL_OK;
}

static bool all_zero(const uint8_t *bytes, size_t len) {
  uint8_t aggregate = 0u;
  for (size_t index = 0u; index < len; ++index)
    aggregate |= bytes[index];
  return aggregate == 0u;
}

static bool append_name_suffix(char *destination, size_t capacity,
                               const char *prefix, const char *suffix) {
  if (destination == NULL || capacity == 0u || prefix == NULL ||
      suffix == NULL) {
    return false;
  }
  const size_t prefix_len = strlen(prefix);
  const size_t suffix_len = strlen(suffix);
  if (suffix_len >= capacity || prefix_len > capacity - suffix_len - 1u) {
    return false;
  }
  memcpy(destination, prefix, prefix_len);
  memcpy(destination + prefix_len, suffix, suffix_len + 1u);
  return true;
}

h2_gizclaw_str_t h2_gizclaw_e2e_str(const char *value) {
  return (h2_gizclaw_str_t){
      .data = value,
      .len = value == NULL ? 0u : strlen(value),
  };
}

void h2_gizclaw_e2e_evidence(const char *symbol, const char *stage,
                             int result) {
  printf("H2_GIZCLAW_E2E symbol=%s stage=%s result=%s rc=%d\n",
         symbol == NULL ? "-" : symbol, stage == NULL ? "-" : stage,
         result == H2_PAL_OK ? "PASS" : "FAIL", result);
}

static int provider_call(void *user, h2_gizclaw_rpc_method_t method,
                         h2_gizclaw_rpc_bytes_t request_payload,
                         h2_gizclaw_rpc_provider_response_t *out_response) {
  h2_gizclaw_e2e_actor_t *actor = user;
  if (actor == NULL || out_response == NULL ||
      (request_payload.data == NULL && request_payload.len != 0u)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_response, 0, sizeof(*out_response));
  if (method == H2_GIZCLAW_RPC_CLIENT_INFO_GET) {
    actor->client_info_requested = true;
    out_response->payload = (h2_gizclaw_rpc_bytes_t){
        .data = s_client_info_response,
        .len = sizeof(s_client_info_response),
    };
    h2_gizclaw_e2e_evidence("H2_GIZCLAW_RPC_CLIENT_INFO_GET", "reverse-rpc",
                            H2_PAL_OK);
    return H2_PAL_OK;
  }
  if (method == H2_GIZCLAW_RPC_CLIENT_IDENTIFIERS_GET) {
    actor->client_identifiers_requested = true;
    out_response->payload = (h2_gizclaw_rpc_bytes_t){
        .data = s_client_identifiers_response,
        .len = sizeof(s_client_identifiers_response),
    };
    h2_gizclaw_e2e_evidence("H2_GIZCLAW_RPC_CLIENT_IDENTIFIERS_GET",
                            "reverse-rpc", H2_PAL_OK);
    return H2_PAL_OK;
  }
  out_response->has_error = true;
  out_response->error_code = H2_GIZCLAW_RPC_ERROR_METHOD_NOT_FOUND;
  return H2_PAL_OK;
}

static bool cancel_requested(void *user) {
  const h2_gizclaw_e2e_fixture_t *fixture = user;
  if (fixture == NULL || fixture->cancel_requested ||
      (fixture->config->should_stop != NULL &&
       fixture->config->should_stop(fixture->config->should_stop_user))) {
    return true;
  }
  uint64_t now_ms = 0u;
  return fixture->time == NULL ||
         h2_pal_time_get_monotonic_ms(fixture->time, &now_ms) != H2_PAL_OK ||
         now_ms >= fixture->deadline_ms;
}

static int actor_create_identity(h2_gizclaw_e2e_fixture_t *fixture,
                                 h2_gizclaw_e2e_actor_t *actor) {
  h2_pal_x25519_keypair_t keypair;
  memset(&keypair, 0, sizeof(keypair));
  int rc = h2_pal_crypto_x25519_keypair_generate(fixture->crypto, &keypair);
  if (rc != H2_PAL_OK ||
      all_zero(keypair.private_key.bytes, sizeof(keypair.private_key.bytes)) ||
      all_zero(keypair.public_key.bytes, sizeof(keypair.public_key.bytes))) {
    memset(&keypair, 0, sizeof(keypair));
    return rc == H2_PAL_OK ? H2_PAL_ERR_INVALID_STATE : rc;
  }
  rc = key_to_base58(keypair.private_key.bytes,
                     sizeof(keypair.private_key.bytes), actor->private_key,
                     sizeof(actor->private_key));
  if (rc == H2_PAL_OK) {
    rc = key_to_base58(keypair.public_key.bytes,
                       sizeof(keypair.public_key.bytes), actor->public_key,
                       sizeof(actor->public_key));
  }
  memset(&keypair, 0, sizeof(keypair));
  return rc;
}

static int actor_connect(h2_gizclaw_e2e_fixture_t *fixture,
                         h2_gizclaw_e2e_actor_t *actor, const char *stage) {
  bool connected_now = false;
  h2_gizclaw_config_t config = {
      .server_endpoint = h2_gizclaw_e2e_str(fixture->endpoint),
      .private_key = h2_gizclaw_e2e_str(actor->private_key),
      .cipher_mode = H2_GIZCLAW_CIPHER_CHACHA20_POLY1305,
      .connect_timeout_ms = H2_GIZCLAW_E2E_CONNECT_TIMEOUT_MS,
      .write_timeout_ms = H2_GIZCLAW_E2E_WRITE_TIMEOUT_MS,
      .allocator = fixture->allocator,
      .http = fixture->http,
      .webrtc = fixture->webrtc,
      .crypto = fixture->crypto,
      .time = fixture->time,
      .log = fixture->log,
      .rpc_provider = provider_call,
      .rpc_provider_user = actor,
      .cancel_requested = cancel_requested,
      .cancel_user = fixture,
  };
  int rc = h2_gizclaw_client_init(&config, &actor->client);
  h2_gizclaw_e2e_evidence("h2_gizclaw_client_init", stage, rc);
  if (rc == H2_PAL_OK) {
    rc = h2_gizclaw_client_connect(actor->client);
    h2_gizclaw_e2e_evidence("h2_gizclaw_client_connect", stage, rc);
  }
  if (rc == H2_PAL_OK) {
    connected_now = true;
    actor->peer_delete_required = true;
  }
  if (rc == H2_PAL_OK) {
    h2_gizclaw_registration_result_t registration = {0};
    rc = h2_gizclaw_client_register(actor->client, fixture->registration_token,
                                    &registration);
    h2_gizclaw_e2e_evidence("h2_gizclaw_client_register", stage, rc);
    if (rc == H2_PAL_OK)
      actor->registered = true;
    if (rc == H2_PAL_OK && fixture->runtime_profile_name[0] == '\0') {
      (void)snprintf(fixture->runtime_profile_name,
                     sizeof(fixture->runtime_profile_name), "%s",
                     registration.runtime_profile_name);
    } else if (rc == H2_PAL_OK && strcmp(registration.runtime_profile_name,
                                         fixture->runtime_profile_name) != 0) {
      rc = H2_PAL_ERR_INVALID_STATE;
    }
  }
  if (rc != H2_PAL_OK) {
    if (connected_now)
      return rc;
    h2_gizclaw_client_deinit(actor->client);
    actor->client = NULL;
    return rc;
  }
  return H2_PAL_OK;
}

int h2_gizclaw_e2e_fixture_init(h2_gizclaw_e2e_fixture_t *fixture,
                                h2_runtime_t *runtime,
                                const h2_gizclaw_e2e_config_t *config,
                                uint32_t suite_timeout_ms) {
  if (fixture == NULL || runtime == NULL || config == NULL ||
      suite_timeout_ms == 0u || runtime->mem == NULL ||
      runtime->crypto == NULL || runtime->http == NULL ||
      runtime->log == NULL || runtime->time == NULL || runtime->sync == NULL ||
      runtime->webrtc == NULL || config->server_endpoint.data == NULL ||
      config->server_endpoint.len == 0u ||
      config->server_endpoint.len >= H2_GIZCLAW_E2E_ENDPOINT_CAPACITY ||
      config->registration_token.data == NULL ||
      config->registration_token.len == 0u ||
      config->registration_token.len > H2_GIZCLAW_E2E_REGISTRATION_TOKEN_MAX ||
      memchr(config->server_endpoint.data, '\0', config->server_endpoint.len) !=
          NULL ||
      memchr(config->registration_token.data, '\0',
             config->registration_token.len) != NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(fixture, 0, sizeof(*fixture));
  fixture->runtime = runtime;
  fixture->config = config;
  fixture->allocator = runtime->mem;
  fixture->crypto = runtime->crypto;
  fixture->http = runtime->http;
  fixture->log = runtime->log;
  fixture->time = runtime->time;
  fixture->webrtc = runtime->webrtc;
  memcpy(fixture->endpoint, config->server_endpoint.data,
         config->server_endpoint.len);
  fixture->endpoint[config->server_endpoint.len] = '\0';
  fixture->registration_token =
      h2_pal_mem_alloc(fixture->allocator, config->registration_token.len + 1u);
  if (fixture->registration_token == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  memcpy(fixture->registration_token, config->registration_token.data,
         config->registration_token.len);
  fixture->registration_token[config->registration_token.len] = '\0';
  fixture->pcm = config->voice_pcm_s16le_16khz_mono;
  fixture->pcm_len = config->voice_pcm_len;

  int rc = h2_pal_time_get_monotonic_ms(fixture->time, &fixture->started_ms);
  if (rc == H2_PAL_OK && UINT64_MAX - fixture->started_ms < suite_timeout_ms) {
    rc = H2_PAL_ERR_INVALID_ARG;
  }
  if (rc != H2_PAL_OK) {
    memset(fixture->registration_token, 0, config->registration_token.len + 1u);
    h2_pal_mem_free(fixture->allocator, fixture->registration_token);
    fixture->registration_token = NULL;
    return rc;
  }
  fixture->deadline_ms = fixture->started_ms + suite_timeout_ms;
  uint8_t run_id[8];
  rc = h2_pal_crypto_random(fixture->crypto, run_id, sizeof(run_id));
  if (rc != H2_PAL_OK || all_zero(run_id, sizeof(run_id))) {
    memset(fixture->registration_token, 0, config->registration_token.len + 1u);
    h2_pal_mem_free(fixture->allocator, fixture->registration_token);
    fixture->registration_token = NULL;
    return rc == H2_PAL_OK ? H2_PAL_ERR_INVALID_STATE : rc;
  }
  char run_hex[sizeof(run_id) * 2u + 1u];
  for (size_t index = 0u; index < sizeof(run_id); ++index) {
    static const char hex[] = "0123456789abcdef";
    run_hex[index * 2u] = hex[run_id[index] >> 4u];
    run_hex[index * 2u + 1u] = hex[run_id[index] & 0x0fu];
  }
  run_hex[sizeof(run_id) * 2u] = '\0';
  (void)snprintf(fixture->run_prefix, sizeof(fixture->run_prefix), "h2e2e-%s",
                 run_hex);
  if (!append_name_suffix(fixture->workspace_name,
                          sizeof(fixture->workspace_name), fixture->run_prefix,
                          "-workspace") ||
      !append_name_suffix(fixture->pet_name, sizeof(fixture->pet_name),
                          fixture->run_prefix, "-pet")) {
    memset(fixture->registration_token, 0, config->registration_token.len + 1u);
    h2_pal_mem_free(fixture->allocator, fixture->registration_token);
    fixture->registration_token = NULL;
    return H2_PAL_ERR_INVALID_STATE;
  }
  rc = observer_init(fixture->webrtc, runtime->sync, runtime->mem);
  if (rc != H2_PAL_OK) {
    memset(fixture->registration_token, 0, config->registration_token.len + 1u);
    h2_pal_mem_free(fixture->allocator, fixture->registration_token);
    fixture->registration_token = NULL;
    return rc;
  }
  fixture->webrtc = &s_webrtc_observer.api;
  return H2_PAL_OK;
}

int h2_gizclaw_e2e_fixture_connect_actors(h2_gizclaw_e2e_fixture_t *fixture,
                                          size_t actor_count) {
  if (fixture == NULL || actor_count == 0u ||
      actor_count > H2_GIZCLAW_E2E_ACTOR_COUNT) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  int rc = H2_PAL_OK;
  for (size_t index = 0u; index < actor_count; ++index) {
    rc = actor_create_identity(fixture, &fixture->actors[index]);
    if (rc == H2_PAL_OK)
      rc = actor_connect(fixture, &fixture->actors[index], "registration");
    if (rc != H2_PAL_OK)
      return rc;
  }
  return H2_PAL_OK;
}

int h2_gizclaw_e2e_fixture_transfer_actor_to_service(
    h2_gizclaw_e2e_fixture_t *fixture, h2_gizclaw_e2e_actor_role_t role,
    h2_gizclaw_config_t *out_config) {
  if (fixture == NULL || out_config == NULL ||
      (unsigned int)role > H2_GIZCLAW_E2E_GROUP_MEMBER) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_gizclaw_e2e_actor_t *actor = &fixture->actors[role];
  if (actor->client == NULL || !actor->registered ||
      !actor->peer_delete_required) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  const int rc = h2_gizclaw_client_close(actor->client);
  h2_gizclaw_e2e_evidence("h2_gizclaw_client_close", "service-transfer", rc);
  h2_gizclaw_client_deinit(actor->client);
  actor->client = NULL;
  actor->registered = false;
  if (rc != H2_PAL_OK)
    return rc;

  *out_config = (h2_gizclaw_config_t){
      .server_endpoint = h2_gizclaw_e2e_str(fixture->endpoint),
      .private_key = h2_gizclaw_e2e_str(actor->private_key),
      .cipher_mode = H2_GIZCLAW_CIPHER_CHACHA20_POLY1305,
      .connect_timeout_ms = H2_GIZCLAW_E2E_CONNECT_TIMEOUT_MS,
      .write_timeout_ms = H2_GIZCLAW_E2E_WRITE_TIMEOUT_MS,
      .allocator = fixture->allocator,
      .http = fixture->http,
      .webrtc = fixture->webrtc,
      .crypto = fixture->crypto,
      .time = fixture->time,
      .log = fixture->log,
      .rpc_provider = provider_call,
      .rpc_provider_user = actor,
      .cancel_requested = cancel_requested,
      .cancel_user = fixture,
  };
  return H2_PAL_OK;
}

int h2_gizclaw_e2e_fixture_reconnect_actor(h2_gizclaw_e2e_fixture_t *fixture,
                                           h2_gizclaw_e2e_actor_role_t role) {
  if (fixture == NULL || (unsigned int)role > H2_GIZCLAW_E2E_GROUP_MEMBER) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_gizclaw_e2e_actor_t *actor = &fixture->actors[role];
  if (!actor->registered)
    return H2_PAL_ERR_INVALID_STATE;
  int rc = H2_PAL_OK;
  if (actor->client != NULL)
    rc = h2_gizclaw_client_close(actor->client);
  h2_gizclaw_e2e_evidence("h2_gizclaw_client_close", "reconnect", rc);
  h2_gizclaw_client_deinit(actor->client);
  actor->client = NULL;
  actor->registered = false;
  const int connect_rc = actor_connect(fixture, actor, "reconnect");
  return rc == H2_PAL_OK ? connect_rc : rc;
}

bool h2_gizclaw_e2e_fixture_has_time(const h2_gizclaw_e2e_fixture_t *fixture,
                                     uint32_t required_ms) {
  if (fixture == NULL || fixture->time == NULL)
    return false;
  uint64_t now_ms = 0u;
  return h2_pal_time_get_monotonic_ms(fixture->time, &now_ms) == H2_PAL_OK &&
         now_ms <= fixture->deadline_ms &&
         fixture->deadline_ms - now_ms >= required_ms;
}

int h2_gizclaw_e2e_fixture_set_deadline(h2_gizclaw_e2e_fixture_t *fixture,
                                        uint32_t timeout_ms) {
  if (fixture == NULL || fixture->time == NULL || timeout_ms == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  uint64_t now_ms = 0u;
  int rc = h2_pal_time_get_monotonic_ms(fixture->time, &now_ms);
  if (rc == H2_PAL_OK && UINT64_MAX - now_ms < timeout_ms)
    rc = H2_PAL_ERR_INVALID_ARG;
  if (rc == H2_PAL_OK)
    fixture->deadline_ms = now_ms + timeout_ms;
  return rc;
}

int h2_gizclaw_e2e_fixture_poll(h2_gizclaw_e2e_fixture_t *fixture,
                                h2_gizclaw_e2e_actor_role_t role,
                                uint32_t duration_ms) {
  if (fixture == NULL || (unsigned int)role > H2_GIZCLAW_E2E_GROUP_MEMBER ||
      fixture->actors[role].client == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  uint64_t start_ms = 0u;
  int rc = h2_pal_time_get_monotonic_ms(fixture->time, &start_ms);
  while (rc == H2_PAL_OK) {
    rc = h2_gizclaw_client_poll(fixture->actors[role].client, 50);
    if (rc != H2_PAL_OK)
      break;
    uint64_t now_ms = 0u;
    rc = h2_pal_time_get_monotonic_ms(fixture->time, &now_ms);
    if (rc != H2_PAL_OK ||
        h2_pal_time_elapsed_ms(start_ms, now_ms) >= duration_ms) {
      break;
    }
  }
  return rc;
}

static void keep_first_failure(int candidate, int *result) {
  if (*result == H2_PAL_OK && candidate != H2_PAL_OK)
    *result = candidate;
}

static int find_group_member_for_cleanup(h2_gizclaw_e2e_fixture_t *fixture,
                                         h2_gizclaw_client_t *owner) {
  if (fixture->friend_group_member_id[0] != '\0')
    return H2_PAL_OK;
  h2_gizclaw_friend_group_member_page_t members = {0};
  int rc = h2_gizclaw_client_friend_group_members_list(
      owner, h2_gizclaw_e2e_str(fixture->friend_group_name),
      (h2_gizclaw_str_t){0}, 64u, &members);
  if (rc == H2_PAL_OK) {
    rc = H2_PAL_ERR_NOT_FOUND;
    for (size_t index = 0u; index < members.count; ++index) {
      const h2_gizclaw_friend_group_member_t *member = &members.items[index];
      if (member->id != NULL && member->peer_public_key != NULL &&
          strcmp(member->peer_public_key,
                 fixture->actors[H2_GIZCLAW_E2E_GROUP_MEMBER].public_key) ==
              0) {
        const int written =
            snprintf(fixture->friend_group_member_id,
                     sizeof(fixture->friend_group_member_id), "%s", member->id);
        rc = written > 0 &&
                     (size_t)written < sizeof(fixture->friend_group_member_id)
                 ? H2_PAL_OK
                 : H2_PAL_ERR_TRUNCATED;
        break;
      }
    }
  }
  h2_gizclaw_friend_group_member_page_deinit(owner, &members);
  return rc;
}

int h2_gizclaw_e2e_fixture_cleanup(h2_gizclaw_e2e_fixture_t *fixture) {
  if (fixture == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  int result = H2_PAL_OK;
  h2_gizclaw_client_t *owner = fixture->actors[H2_GIZCLAW_E2E_OWNER].client;
  h2_gizclaw_client_t *friend_client =
      fixture->actors[H2_GIZCLAW_E2E_FRIEND].client;
  if (friend_client != NULL && fixture->friend_invite_created) {
    int rc = h2_gizclaw_client_friend_invite_token_clear(friend_client);
    h2_gizclaw_e2e_evidence("h2_gizclaw_client_friend_invite_token_clear",
                            "cleanup", rc);
    keep_first_failure(rc, &result);
    if (rc == H2_PAL_OK)
      fixture->friend_invite_created = false;
  }
  if (owner != NULL && fixture->friend_group_invite_created &&
      fixture->friend_group_name[0] != '\0') {
    int rc = h2_gizclaw_client_friend_group_invite_token_clear(
        owner, h2_gizclaw_e2e_str(fixture->friend_group_name));
    h2_gizclaw_e2e_evidence("h2_gizclaw_client_friend_group_invite_token_clear",
                            "cleanup", rc);
    keep_first_failure(rc, &result);
    if (rc == H2_PAL_OK)
      fixture->friend_group_invite_created = false;
  }
  if (owner != NULL && fixture->friend_group_member_joined &&
      fixture->friend_group_member_id[0] == '\0') {
    int rc = find_group_member_for_cleanup(fixture, owner);
    h2_gizclaw_e2e_evidence("h2_gizclaw_client_friend_group_members_list",
                            "cleanup", rc);
    keep_first_failure(rc, &result);
  }
  if (owner != NULL && fixture->friend_group_member_joined &&
      fixture->friend_group_member_id[0] != '\0') {
    h2_gizclaw_friend_group_member_t member = {0};
    int rc = h2_gizclaw_client_friend_group_member_delete(
        owner, h2_gizclaw_e2e_str(fixture->friend_group_name),
        h2_gizclaw_e2e_str(fixture->friend_group_member_id), &member);
    h2_gizclaw_friend_group_member_deinit(owner, &member);
    h2_gizclaw_e2e_evidence("h2_gizclaw_client_friend_group_member_delete",
                            "cleanup", rc);
    keep_first_failure(rc, &result);
    if (rc == H2_PAL_OK)
      fixture->friend_group_member_joined = false;
  }
  if (owner != NULL && fixture->friend_group_created) {
    h2_gizclaw_friend_group_t group = {0};
    int rc = h2_gizclaw_client_friend_group_delete(
        owner, h2_gizclaw_e2e_str(fixture->friend_group_name), &group);
    h2_gizclaw_friend_group_deinit(owner, &group);
    h2_gizclaw_e2e_evidence("h2_gizclaw_client_friend_group_delete", "cleanup",
                            rc);
    keep_first_failure(rc, &result);
    if (rc == H2_PAL_OK)
      fixture->friend_group_created = false;
  }
  if (owner != NULL && fixture->friendship_created) {
    h2_gizclaw_friend_t friend_value = {0};
    int rc = h2_gizclaw_client_friend_delete(
        owner, h2_gizclaw_e2e_str(fixture->friend_id), &friend_value);
    h2_gizclaw_friend_deinit(owner, &friend_value);
    h2_gizclaw_e2e_evidence("h2_gizclaw_client_friend_delete", "cleanup", rc);
    keep_first_failure(rc, &result);
    if (rc == H2_PAL_OK)
      fixture->friendship_created = false;
  }
  if (owner != NULL && fixture->contact_created) {
    h2_gizclaw_contact_t contact = {0};
    int rc = h2_gizclaw_client_contact_delete(
        owner, h2_gizclaw_e2e_str(fixture->contact_name), &contact);
    h2_gizclaw_contact_deinit(owner, &contact);
    h2_gizclaw_e2e_evidence("h2_gizclaw_client_contact_delete", "cleanup", rc);
    keep_first_failure(rc, &result);
    if (rc == H2_PAL_OK)
      fixture->contact_created = false;
  }
  if (owner != NULL && fixture->pet_created) {
    h2_gizclaw_pet_t pet = {0};
    int rc = h2_gizclaw_client_pet_delete(
        owner, h2_gizclaw_e2e_str(fixture->pet_name), &pet);
    h2_gizclaw_pet_deinit(owner, &pet);
    h2_gizclaw_e2e_evidence("h2_gizclaw_client_pet_delete", "cleanup", rc);
    keep_first_failure(rc, &result);
    if (rc == H2_PAL_OK)
      fixture->pet_created = false;
  }
  if (owner != NULL && fixture->workspace_created) {
    h2_gizclaw_workspace_t workspace = {0};
    int rc = h2_gizclaw_client_workspace_delete(
        owner, h2_gizclaw_e2e_str(fixture->workspace_name), &workspace);
    h2_gizclaw_workspace_deinit(owner, &workspace);
    h2_gizclaw_e2e_evidence("h2_gizclaw_client_workspace_delete", "cleanup",
                            rc);
    keep_first_failure(rc, &result);
    if (rc == H2_PAL_OK)
      fixture->workspace_created = false;
  }
  for (size_t index = 0u; index < H2_GIZCLAW_E2E_ACTOR_COUNT; ++index) {
    h2_gizclaw_e2e_actor_t *actor = &fixture->actors[index];
    if (!actor->peer_delete_required)
      continue;
    if (actor->client == NULL) {
      int rc = actor_connect(fixture, actor, "cleanup-reconnect");
      keep_first_failure(rc, &result);
      if (rc != H2_PAL_OK)
        continue;
    }
    int rc = h2_gizclaw_client_delete_peer(actor->client);
    if (rc == H2_PAL_ERR_IO &&
        h2_gizclaw_client_poll(actor->client, 0) == H2_PAL_ERR_CLOSED) {
      /* Deleting the current Peer is terminal. The E2E service can close the
       * association immediately after its acknowledgement reaches the wire,
       * so a confirmed terminal client is the successful postcondition. */
      rc = H2_PAL_OK;
    }
    h2_gizclaw_e2e_evidence("h2_gizclaw_client_delete_peer", "cleanup", rc);
    keep_first_failure(rc, &result);
    if (rc == H2_PAL_OK)
      actor->peer_delete_requested = true;
    if (rc == H2_PAL_OK)
      actor->peer_delete_required = false;
    if (rc == H2_PAL_OK)
      actor->registered = false;
  }
  return result;
}

static size_t emit_retained_resource(const h2_gizclaw_e2e_fixture_t *fixture,
                                     const char *resource, bool retained) {
  if (!retained)
    return 0u;
  char message[H2_PAL_LOG_MESSAGE_MAX];
  (void)snprintf(message, sizeof(message),
                 "stage=recovery resource=%s retained=true", resource);
  (void)h2_pal_log_write(fixture->log, H2_PAL_LOG_WARN, "gizclaw-e2e", message);
  return 1u;
}

size_t h2_gizclaw_e2e_fixture_emit_recovery_ledger(
    const h2_gizclaw_e2e_fixture_t *fixture) {
  if (fixture == NULL || fixture->log == NULL)
    return 0u;
  size_t retained = 0u;
  retained +=
      emit_retained_resource(fixture, "workspace", fixture->workspace_created);
  retained += emit_retained_resource(fixture, "pet", fixture->pet_created);
  retained +=
      emit_retained_resource(fixture, "contact", fixture->contact_created);
  retained += emit_retained_resource(fixture, "friendship",
                                     fixture->friendship_created);
  retained += emit_retained_resource(fixture, "friend-invite",
                                     fixture->friend_invite_created);
  retained += emit_retained_resource(fixture, "friend-group",
                                     fixture->friend_group_created);
  retained += emit_retained_resource(fixture, "friend-group-invite",
                                     fixture->friend_group_invite_created);
  retained += emit_retained_resource(fixture, "friend-group-member",
                                     fixture->friend_group_member_joined);
  for (size_t index = 0u; index < H2_GIZCLAW_E2E_ACTOR_COUNT; ++index) {
    retained += emit_retained_resource(
        fixture, "peer", fixture->actors[index].peer_delete_required);
  }
  return retained;
}

void h2_gizclaw_e2e_fixture_deinit(h2_gizclaw_e2e_fixture_t *fixture) {
  if (fixture == NULL)
    return;
  for (size_t index = 0u; index < H2_GIZCLAW_E2E_ACTOR_COUNT; ++index) {
    h2_gizclaw_e2e_actor_t *actor = &fixture->actors[index];
    if (actor->client != NULL)
      (void)h2_gizclaw_client_close(actor->client);
    h2_gizclaw_client_deinit(actor->client);
    actor->client = NULL;
    memset(actor->private_key, 0, sizeof(actor->private_key));
  }
  fixture->pcm = NULL;
  fixture->pcm_len = 0u;
  if (fixture->registration_token != NULL) {
    memset(fixture->registration_token, 0,
           fixture->config->registration_token.len + 1u);
    h2_pal_mem_free(fixture->allocator, fixture->registration_token);
    fixture->registration_token = NULL;
  }
  fixture->http = NULL;
  fixture->webrtc = NULL;
  observer_deinit();
}
