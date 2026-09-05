#include "h2_peer_portable_backend.h"
#include "h2_peer_internal.h"

#include "peer.h"
#include "peer_connection.h"
#include "sctp.h"
#include "utils.h"

#include <errno.h>
#include <stdatomic.h>
#include <string.h>

static size_t h2_peer_portable_live_connections;
static atomic_flag h2_peer_portable_global_lock = ATOMIC_FLAG_INIT;

static void h2_peer_portable_lock_globals(void) {
    while (atomic_flag_test_and_set_explicit(&h2_peer_portable_global_lock,
                                             memory_order_acquire)) {
    }
}

static void h2_peer_portable_unlock_globals(void) {
    atomic_flag_clear_explicit(&h2_peer_portable_global_lock,
                               memory_order_release);
}

static h2_pal_result_t
h2_peer_portable_global_acquire(const h2_pal_mem_api_t *mem,
                                const h2_pal_crypto_api_t *crypto,
                                const h2_pal_log_api_t *log) {
  (void)log;
  h2_peer_portable_lock_globals();
  h2_pal_result_t result = peer_init(mem, crypto);
  if (result == H2_PAL_OK) {
    h2_peer_portable_live_connections++;
  }
  h2_peer_portable_unlock_globals();
  if (result != H2_PAL_OK) {
    H2_PEER_LOGE(log, "libSRTP initialization failed: %d", (int)result);
  }
  return result;
}

static void h2_peer_portable_global_release(void) {
    h2_peer_portable_lock_globals();
    if (h2_peer_portable_live_connections != 0u) {
        h2_peer_portable_live_connections--;
        /* peer_deinit releases one ref; libSRTP shuts down on the final ref. */
        peer_deinit();
    }
    h2_peer_portable_unlock_globals();
}

static void *h2_peer_portable_alloc(h2_pal_webrtc_peer_t *peer, size_t len) {
    void *ptr = h2_pal_mem_alloc(peer->owner->config.mem, len);
    if (ptr != NULL) {
        memset(ptr, 0, len);
    }
    return ptr;
}

static void h2_peer_portable_free(h2_pal_webrtc_peer_t *peer, void *ptr) {
    h2_pal_mem_free(peer->owner->config.mem, ptr);
}

static h2_pal_webrtc_channel_t *
h2_peer_portable_find_channel(h2_pal_webrtc_peer_t *peer, uint16_t stream_id) {
    for (h2_pal_webrtc_channel_t *channel = peer->channels; channel != NULL;
         channel = channel->next) {
        if (channel->info.stream_id == stream_id) {
            return channel;
        }
    }
    return NULL;
}

static void h2_peer_portable_emit_peer_state(h2_pal_webrtc_peer_t *peer,
                                             h2_pal_webrtc_peer_state_t state) {
    if (peer->closed || peer->state == state) {
        return;
    }
    h2_peer_webrtc_emit_peer_state(peer, state);
}

static void
h2_peer_portable_emit_channel_state(h2_pal_webrtc_channel_t *channel,
                                    h2_pal_webrtc_channel_state_t state) {
    h2_peer_webrtc_emit_channel_state(channel, state);
}

static DecpChannelType
h2_peer_portable_channel_type(const h2_pal_webrtc_channel_t *channel) {
    if (channel->info.reliable) {
        return channel->info.ordered ? DATA_CHANNEL_RELIABLE
                                     : DATA_CHANNEL_RELIABLE_UNORDERED;
    }
    return channel->info.ordered
               ? DATA_CHANNEL_PARTIAL_RELIABLE_REXMIT
               : DATA_CHANNEL_PARTIAL_RELIABLE_REXMIT_UNORDERED;
}

static void h2_peer_portable_on_local_sdp(char *sdp, void *user) {
    h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)user;
    if (peer == NULL || peer->closed || sdp == NULL) {
        return;
    }
    h2_pal_webrtc_str_t value = {
        .data = sdp,
        .len = strlen(sdp),
    };
    h2_peer_webrtc_emit_local_sdp(peer, H2_PAL_WEBRTC_SDP_OFFER, value);
}

static void h2_peer_portable_on_opus(uint8_t *data, size_t len, void *user) {
    h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)user;
    if (peer == NULL || peer->closed || (data == NULL && len != 0u) ||
        len > H2_PAL_WEBRTC_OPUS_MAX_PACKET_SIZE) {
        return;
    }
    h2_peer_webrtc_emit_opus_frame(peer, data, len);
}

static void h2_peer_portable_on_state(PeerConnectionState state, void *user) {
    h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)user;
    if (peer == NULL || peer->closed ||
        peer->stream_reset_failure != H2_PAL_OK) {
        return;
    }
    switch (state) {
    case PEER_CONNECTION_NEW:
        h2_peer_portable_emit_peer_state(peer, H2_PAL_WEBRTC_PEER_NEW);
        break;
    case PEER_CONNECTION_CHECKING:
    case PEER_CONNECTION_CONNECTED:
        h2_peer_portable_emit_peer_state(peer, H2_PAL_WEBRTC_PEER_CONNECTING);
        break;
    case PEER_CONNECTION_COMPLETED:
        h2_peer_portable_emit_peer_state(peer, H2_PAL_WEBRTC_PEER_CONNECTED);
        break;
    case PEER_CONNECTION_DISCONNECTED:
        h2_peer_portable_emit_peer_state(peer, H2_PAL_WEBRTC_PEER_DISCONNECTED);
        break;
    case PEER_CONNECTION_FAILED:
        h2_peer_webrtc_on_sctp_closed(peer);
        h2_peer_portable_emit_peer_state(peer, H2_PAL_WEBRTC_PEER_FAILED);
        break;
    case PEER_CONNECTION_CLOSED:
        h2_peer_webrtc_on_sctp_closed(peer);
        h2_peer_portable_emit_peer_state(peer, H2_PAL_WEBRTC_PEER_DISCONNECTED);
        break;
    }
}

static h2_pal_result_t h2_peer_portable_on_message(
    char *message, size_t len, void *user, uint16_t stream_id, int is_text) {
    h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)user;
    if (peer == NULL || peer->closed) {
        return H2_PAL_OK;
    }
    h2_pal_webrtc_channel_t *channel =
        h2_peer_portable_find_channel(peer, stream_id);
    if (channel == NULL || !channel->open) {
        return H2_PAL_OK;
    }
    return h2_peer_webrtc_emit_channel_message(
        peer, channel, (const uint8_t *)message, len, is_text);
}

static int h2_peer_portable_on_remote_channel(const SctpRemoteChannel *remote,
                                              void *user) {
    h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)user;
    if (peer == NULL || remote == NULL) {
        return -1;
    }
    const h2_pal_webrtc_str_t label = {
        .data = remote->label,
        .len = remote->label_len,
    };
    return h2_peer_webrtc_on_remote_channel_open(
               peer, label, remote->sid, !remote->unordered,
               remote->reliability == H2_PAL_SCTP_RELIABILITY_RELIABLE) ==
                   H2_PAL_OK
               ? 0
               : -1;
}

static void h2_peer_portable_on_local_channel_open(uint16_t stream_id,
                                                   void *user) {
    h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)user;
    if (peer == NULL || peer->closed) {
        return;
    }
    h2_pal_webrtc_channel_t *channel =
        h2_peer_portable_find_channel(peer, stream_id);
    if (channel == NULL || channel->remote_created || !channel->wire_opened ||
        channel->open) {
        return;
    }
    channel->open = 1;
    h2_peer_portable_emit_channel_state(channel, H2_PAL_WEBRTC_CHANNEL_OPEN);
}

h2_pal_result_t
h2_peer_portable_channel_open(h2_pal_webrtc_channel_t *channel) {
    if (channel == NULL || channel->owner == NULL ||
        channel->owner->production_pc == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    int result = peer_connection_create_datachannel_sid(
        (PeerConnection *)channel->owner->production_pc,
        h2_peer_portable_channel_type(channel), 0u, 0u, channel->label, "",
        channel->info.stream_id);
    if (result < 0) {
        return result == H2_PAL_ERR_WOULD_BLOCK || errno == EAGAIN ||
                       errno == EWOULDBLOCK
                   ? H2_PAL_ERR_WOULD_BLOCK
                   : H2_PAL_ERR_IO;
    }
    return H2_PAL_OK;
}

static void h2_peer_portable_on_sctp_open(void *user) {
    h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)user;
    if (peer == NULL || peer->closed) {
        return;
    }
    peer->production_sctp_open = 1;
    for (;;) {
        h2_pal_webrtc_channel_t *channel = peer->channels;
        while (channel != NULL &&
               (channel->remote_created || channel->wire_opened)) {
            channel = channel->next;
        }
        if (channel == NULL) {
            return;
        }
        const uint16_t stream_id = channel->info.stream_id;
        const uint32_t generation = channel->generation;
        h2_pal_result_t result = h2_peer_portable_channel_open(channel);
        if (result != H2_PAL_OK && result != H2_PAL_ERR_WOULD_BLOCK) {
            h2_peer_portable_emit_peer_state(peer, H2_PAL_WEBRTC_PEER_FAILED);
            return;
        }
        if (result == H2_PAL_ERR_WOULD_BLOCK) {
            return;
        }
        h2_pal_webrtc_channel_t *current =
            h2_peer_portable_find_channel(peer, stream_id);
        if (peer->closed || current != channel ||
            current->generation != generation) {
            return;
        }
        channel->wire_opened = 1;
    }
}

static void h2_peer_portable_on_sctp_close(void *user) {
    h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)user;
    if (peer == NULL) {
        return;
    }
    peer->production_sctp_open = 0;
    h2_peer_webrtc_on_sctp_closed(peer);
}

static void
h2_peer_portable_on_stream_reset(const h2_pal_sctp_stream_reset_event_t *event,
                                 void *user) {
    h2_peer_webrtc_on_stream_reset((h2_pal_webrtc_peer_t *)user, event);
}

static h2_pal_result_t
h2_peer_portable_create_connection(h2_pal_webrtc_peer_t *peer) {
    h2_pal_result_t result = h2_peer_portable_global_acquire(
        peer->owner->config.mem, peer->owner->config.crypto,
        peer->owner->config.log);
    if (result != H2_PAL_OK) {
        return result;
    }
    PeerConfiguration config;
    memset(&config, 0, sizeof(config));
    config.log = peer->owner->config.log;
    config.mem = peer->owner->config.mem;
    config.net = peer->owner->config.net;
    config.time = peer->owner->config.time;
    config.crypto = peer->owner->config.crypto;
    config.dtls = peer->owner->config.dtls;
    config.sctp = peer->owner->config.sctp;
    config.audio_codec = CODEC_OPUS;
    config.video_codec = CODEC_NONE;
    config.datachannel = DATA_CHANNEL_BINARY;
    config.onaudiotrack = h2_peer_portable_on_opus;
    config.user_data = peer;
    for (size_t i = 0u; i < peer->ice_server_count; ++i) {
        config.ice_servers[i].urls = peer->ice_servers[i].url;
        config.ice_servers[i].username = peer->ice_servers[i].username;
        config.ice_servers[i].credential = peer->ice_servers[i].credential;
    }
    PeerConnection *connection = peer_connection_create(&config);
    if (connection == NULL) {
        h2_peer_portable_global_release();
        return H2_PAL_ERR_NO_MEMORY;
    }
    peer->production_pc = connection;
    peer_connection_onicecandidate(connection, h2_peer_portable_on_local_sdp);
    peer_connection_oniceconnectionstatechange(connection,
                                               h2_peer_portable_on_state);
    peer_connection_ondatachannel(connection, h2_peer_portable_on_message,
                                  h2_peer_portable_on_sctp_open,
                                  h2_peer_portable_on_sctp_close);
    peer_connection_onstreamreset(connection, h2_peer_portable_on_stream_reset);
    peer_connection_onlocalchannelopen(connection,
                                       h2_peer_portable_on_local_channel_open);
    peer_connection_onremotechannel(connection,
                                    h2_peer_portable_on_remote_channel);
    return H2_PAL_OK;
}

h2_pal_result_t h2_peer_portable_start_offer(h2_pal_webrtc_peer_t *peer) {
    h2_pal_result_t result = h2_peer_portable_create_connection(peer);
    if (result != H2_PAL_OK) {
        return result;
    }
    peer->offer_started = 1;
    h2_peer_portable_emit_peer_state(peer, H2_PAL_WEBRTC_PEER_CONNECTING);
    return peer_connection_create_offer(
               (PeerConnection *)peer->production_pc) == NULL
               ? H2_PAL_ERR_IO
               : H2_PAL_OK;
}

h2_pal_result_t h2_peer_portable_set_remote_sdp(h2_pal_webrtc_peer_t *peer,
                                                h2_pal_webrtc_sdp_type_t type,
                                                h2_pal_webrtc_str_t sdp) {
    if (type != H2_PAL_WEBRTC_SDP_ANSWER || peer->production_pc == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    char *copy = (char *)h2_peer_portable_alloc(peer, sdp.len + 1u);
    if (copy == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memcpy(copy, sdp.data, sdp.len);
    copy[sdp.len] = '\0';
    int set_result = peer_connection_set_remote_description(
        (PeerConnection *)peer->production_pc, copy, SDP_TYPE_ANSWER);
    h2_peer_portable_free(peer, copy);
    if (set_result != 0) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    peer->remote_answer_set = 1;
    return H2_PAL_OK;
}

h2_pal_result_t h2_peer_portable_poll(h2_pal_webrtc_peer_t *peer,
                                      int timeout_ms) {
    if (peer->production_pc == NULL || timeout_ms < 0) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    return peer_connection_loop((PeerConnection *)peer->production_pc,
                                (uint32_t)timeout_ms);
}

int h2_peer_portable_receive_datagram(h2_pal_webrtc_peer_t *peer,
                                      h2_pal_net_addr_t *addr, uint8_t *packet,
                                      size_t packet_cap, uint32_t timeout_ms) {
    if (peer == NULL || peer->production_pc == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    return peer_connection_receive_datagram(
        (PeerConnection *)peer->production_pc, addr, packet, packet_cap,
        timeout_ms);
}

h2_pal_result_t h2_peer_portable_service_datagram(h2_pal_webrtc_peer_t *peer,
                                                  h2_pal_net_addr_t *addr,
                                                  uint8_t *packet,
                                                  size_t packet_len) {
    if (peer == NULL || peer->production_pc == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    return peer_connection_service_datagram(
        (PeerConnection *)peer->production_pc, addr, packet, packet_len);
}

int h2_peer_portable_async_receive_supported(const h2_pal_webrtc_peer_t *peer) {
    return peer != NULL && peer->production_pc != NULL &&
           peer_connection_async_receive_supported(
               (PeerConnection *)peer->production_pc);
}

h2_pal_result_t h2_peer_portable_send_opus(h2_pal_webrtc_peer_t *peer,
                                           const uint8_t *opus,
                                           size_t opus_len) {
    if (peer->production_pc == NULL ||
        peer->state != H2_PAL_WEBRTC_PEER_CONNECTED) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    int result = peer_connection_send_audio(
        (PeerConnection *)peer->production_pc, opus, opus_len);
    if (result >= 0) {
        return H2_PAL_OK;
    }
    return result == H2_PAL_ERR_WOULD_BLOCK ? H2_PAL_ERR_WOULD_BLOCK
                                            : H2_PAL_ERR_IO;
}

h2_pal_result_t h2_peer_portable_channel_send(h2_pal_webrtc_channel_t *channel,
                                              const uint8_t *data, size_t len,
                                              int is_text) {
    if (channel == NULL || channel->owner == NULL ||
        channel->owner->production_pc == NULL || !channel->open) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    Sctp *sctp = (Sctp *)peer_connection_get_sctp(
        (PeerConnection *)channel->owner->production_pc);
    if (sctp == NULL || !sctp_is_connected(sctp)) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    char empty = '\0';
    char *payload = len == 0u ? &empty : (char *)data;
    SctpDataPpid ppid = is_text ? (len == 0u ? PPID_STRING_EMPTY : PPID_STRING)
                                : (len == 0u ? PPID_BINARY_EMPTY : PPID_BINARY);
    errno = 0;
    int result =
        sctp_outgoing_data(sctp, payload, len, ppid, channel->info.stream_id);
    if (result >= 0) {
        return H2_PAL_OK;
    }
    return result == H2_PAL_ERR_WOULD_BLOCK || errno == EAGAIN ||
                   errno == EWOULDBLOCK
               ? H2_PAL_ERR_WOULD_BLOCK
               : H2_PAL_ERR_IO;
}

h2_pal_result_t h2_peer_portable_sctp_is_writable(
    h2_pal_webrtc_peer_t *peer, bool *out_writable) {
    if (out_writable == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_writable = false;
    if (peer == NULL || peer->production_pc == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    return peer_connection_sctp_is_writable(
        (PeerConnection *)peer->production_pc, out_writable);
}

h2_pal_result_t h2_peer_portable_reset_stream(h2_pal_webrtc_peer_t *peer,
                                              uint16_t stream_id) {
    if (peer == NULL || peer->production_pc == NULL ||
        !peer->production_sctp_open) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    return peer_connection_datachannel_close_sid(
        (PeerConnection *)peer->production_pc, stream_id);
}

h2_pal_result_t h2_peer_portable_forget_stream(h2_pal_webrtc_peer_t *peer,
                                               uint16_t stream_id) {
    if (peer == NULL || peer->production_pc == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    return peer_connection_datachannel_forget_sid(
               (PeerConnection *)peer->production_pc, stream_id) == 0
               ? H2_PAL_OK
               : H2_PAL_ERR_NOT_FOUND;
}

void h2_peer_portable_peer_close(h2_pal_webrtc_peer_t *peer) {
    if (peer == NULL || peer->production_pc == NULL) {
        return;
    }
    peer_connection_close((PeerConnection *)peer->production_pc);
    peer_connection_destroy((PeerConnection *)peer->production_pc);
    peer->production_pc = NULL;
    peer->production_sctp_open = 0;
    h2_peer_portable_global_release();
}
