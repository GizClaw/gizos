#include "h2_bk_platform_core.h"

#if defined(CONFIG_KVS_AWS) && CONFIG_KVS_AWS

#include <com/amazonaws/kinesis/video/webrtcclient/Include.h>
#include <os/mem.h>
#include <os/os.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define H2_BK_WEBRTC_EVENT_CAPACITY 32u

struct h2_pal_webrtc_channel {
    struct h2_pal_webrtc_peer *owner;
    PRtcDataChannel rtc;
    h2_pal_webrtc_channel_info_t info;
    char label[MAX_DATA_CHANNEL_NAME_LEN + 1u];
    struct h2_pal_webrtc_channel *next;
};

typedef struct h2_bk_webrtc_event {
    struct h2_bk_webrtc_event *next;
    h2_pal_webrtc_event_t event;
    uint8_t *payload;
    char label[MAX_DATA_CHANNEL_NAME_LEN + 1u];
} h2_bk_webrtc_event_t;

struct h2_pal_webrtc_peer {
    PRtcPeerConnection rtc;
    h2_pal_webrtc_channel_t *channels;
    h2_bk_webrtc_event_t *event_head;
    h2_bk_webrtc_event_t *event_tail;
    size_t event_count;
    beken_mutex_t event_mutex;
    volatile int candidate_gathering_done;
};

static int s_kvs_initialized;

static void *h2_bk_webrtc_alloc(size_t len) {
    void *ptr = os_malloc(len);
    if (ptr != NULL) {
        memset(ptr, 0, len);
    }
    return ptr;
}

static int h2_bk_webrtc_map_status(STATUS status) {
    if (STATUS_SUCCEEDED(status)) {
        return H2_PAL_OK;
    }
    switch (status) {
        case STATUS_NULL_ARG:
        case STATUS_INVALID_ARG:
            return H2_PAL_ERR_INVALID_ARG;
        case STATUS_NOT_ENOUGH_MEMORY:
            return H2_PAL_ERR_NO_MEMORY;
        case STATUS_BUFFER_TOO_SMALL:
            return H2_PAL_ERR_NO_SPACE;
        default:
            return H2_PAL_ERR_IO;
    }
}

static void h2_bk_webrtc_event_release(h2_pal_webrtc_event_t *event) {
    if (event == NULL || event->_private == NULL) {
        return;
    }
    h2_bk_webrtc_event_t *node = event->_private;
    os_free(node->payload);
    os_free(node);
    memset(event, 0, sizeof(*event));
}

static int h2_bk_webrtc_enqueue_event(
    h2_pal_webrtc_peer_t *peer, h2_pal_webrtc_event_kind_t kind,
    h2_pal_webrtc_channel_t *channel, h2_pal_webrtc_channel_state_t state,
    h2_pal_webrtc_peer_state_t peer_state,
    h2_pal_webrtc_sdp_type_t sdp_type, const void *payload,
    size_t payload_len, int is_text) {
    if (peer == NULL || peer->event_mutex == NULL ||
        (payload == NULL && payload_len != 0u) || payload_len == SIZE_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_bk_webrtc_event_t *node = h2_bk_webrtc_alloc(sizeof(*node));
    if (node == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    if (payload_len != 0u) {
        node->payload = h2_bk_webrtc_alloc(payload_len + 1u);
        if (node->payload == NULL) {
            os_free(node);
            return H2_PAL_ERR_NO_MEMORY;
        }
        memcpy(node->payload, payload, payload_len);
    }
    node->event.kind = kind;
    node->event.peer = peer;
    node->event.channel = channel;
    node->event.channel_state = state;
    node->event.peer_state = peer_state;
    node->event.sdp_type = sdp_type;
    node->event.data = node->payload;
    node->event.data_len = payload_len;
    node->event.is_text = is_text;
    if (channel != NULL) {
        node->event.channel_info = channel->info;
        memcpy(node->label, channel->label, channel->info.label.len + 1u);
        node->event.channel_info.label.data = node->label;
    }
    if (kind == H2_PAL_WEBRTC_EVENT_LOCAL_SDP) {
        node->event.sdp.data = (const char *)node->payload;
        node->event.sdp.len = payload_len;
        node->event.data = NULL;
        node->event.data_len = 0u;
    }
    if (rtos_lock_mutex(&peer->event_mutex) != kNoErr) {
        os_free(node->payload);
        os_free(node);
        return H2_PAL_ERR_IO;
    }
    if (peer->event_count >= H2_BK_WEBRTC_EVENT_CAPACITY) {
        (void)rtos_unlock_mutex(&peer->event_mutex);
        os_free(node->payload);
        os_free(node);
        return H2_PAL_ERR_FULL;
    }
    if (peer->event_tail == NULL) {
        peer->event_head = node;
    } else {
        peer->event_tail->next = node;
    }
    peer->event_tail = node;
    ++peer->event_count;
    (void)rtos_unlock_mutex(&peer->event_mutex);
    return H2_PAL_OK;
}

static int h2_bk_webrtc_dequeue_event(h2_pal_webrtc_peer_t *peer,
                                      h2_pal_webrtc_event_t *out_event) {
    if (peer == NULL || out_event == NULL || peer->event_mutex == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (rtos_lock_mutex(&peer->event_mutex) != kNoErr) {
        return H2_PAL_ERR_IO;
    }
    h2_bk_webrtc_event_t *node = peer->event_head;
    if (node != NULL) {
        peer->event_head = node->next;
        if (peer->event_head == NULL) {
            peer->event_tail = NULL;
        }
        --peer->event_count;
    }
    (void)rtos_unlock_mutex(&peer->event_mutex);
    if (node == NULL) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    *out_event = node->event;
    out_event->_private = node;
    out_event->_release = h2_bk_webrtc_event_release;
    return H2_PAL_OK;
}

static h2_pal_webrtc_peer_state_t h2_bk_webrtc_map_state(RTC_PEER_CONNECTION_STATE state) {
    switch (state) {
        case RTC_PEER_CONNECTION_STATE_NEW:
            return H2_PAL_WEBRTC_PEER_NEW;
        case RTC_PEER_CONNECTION_STATE_CONNECTING:
            return H2_PAL_WEBRTC_PEER_CONNECTING;
        case RTC_PEER_CONNECTION_STATE_CONNECTED:
            return H2_PAL_WEBRTC_PEER_CONNECTED;
        case RTC_PEER_CONNECTION_STATE_DISCONNECTED:
            return H2_PAL_WEBRTC_PEER_DISCONNECTED;
        case RTC_PEER_CONNECTION_STATE_FAILED:
            return H2_PAL_WEBRTC_PEER_FAILED;
        case RTC_PEER_CONNECTION_STATE_CLOSED:
            return H2_PAL_WEBRTC_PEER_CLOSED;
        default:
            return H2_PAL_WEBRTC_PEER_CONNECTING;
    }
}

static SDP_TYPE h2_bk_webrtc_map_sdp_type(h2_pal_webrtc_sdp_type_t type) {
    return type == H2_PAL_WEBRTC_SDP_OFFER ? SDP_TYPE_OFFER : SDP_TYPE_ANSWER;
}

static h2_pal_webrtc_channel_t *h2_bk_webrtc_find_channel_by_rtc(
    h2_pal_webrtc_peer_t *peer,
    PRtcDataChannel rtc) {
    if (peer == NULL || rtc == NULL) {
        return NULL;
    }
    for (h2_pal_webrtc_channel_t *channel = peer->channels; channel != NULL; channel = channel->next) {
        if (channel->rtc == rtc) {
            return channel;
        }
    }
    return NULL;
}

static h2_pal_webrtc_channel_t *h2_bk_webrtc_find_channel_by_label(
    h2_pal_webrtc_peer_t *peer,
    const char *label) {
    if (peer == NULL || label == NULL) {
        return NULL;
    }
    for (h2_pal_webrtc_channel_t *channel = peer->channels; channel != NULL; channel = channel->next) {
        if (strcmp(channel->label, label) == 0) {
            return channel;
        }
    }
    return NULL;
}

static void h2_bk_webrtc_link_channel(h2_pal_webrtc_peer_t *peer, h2_pal_webrtc_channel_t *channel) {
    channel->next = peer->channels;
    peer->channels = channel;
}

static h2_pal_webrtc_channel_t *h2_bk_webrtc_alloc_channel(
    h2_pal_webrtc_peer_t *peer,
    const char *label,
    size_t label_len) {
    if (peer == NULL || label == NULL || label_len == 0u || label_len > MAX_DATA_CHANNEL_NAME_LEN) {
        return NULL;
    }
    h2_pal_webrtc_channel_t *channel = (h2_pal_webrtc_channel_t *)h2_bk_webrtc_alloc(sizeof(*channel));
    if (channel == NULL) {
        return NULL;
    }
    channel->owner = peer;
    memcpy(channel->label, label, label_len);
    channel->label[label_len] = '\0';
    channel->info.label.data = channel->label;
    channel->info.label.len = label_len;
    return channel;
}

static void h2_bk_webrtc_on_open(UINT64 customData, PRtcDataChannel rtc) {
    h2_pal_webrtc_channel_t *channel = (h2_pal_webrtc_channel_t *)(uintptr_t)customData;
    if (channel == NULL || channel->owner == NULL) {
        return;
    }
    channel->rtc = rtc;
    if (rtc != NULL) {
        channel->info.stream_id = (uint16_t)rtc->id;
        channel->info.has_stream_id = 1;
    }
    (void)h2_bk_webrtc_enqueue_event(
        channel->owner, H2_PAL_WEBRTC_EVENT_CHANNEL_STATE, channel,
        H2_PAL_WEBRTC_CHANNEL_OPEN, 0, 0, NULL, 0u, 0);
}

static void h2_bk_webrtc_on_message(UINT64 customData, PRtcDataChannel rtc, BOOL isBinary, PBYTE data, UINT32 len) {
    h2_pal_webrtc_channel_t *channel = (h2_pal_webrtc_channel_t *)(uintptr_t)customData;
    if (channel == NULL || channel->owner == NULL) {
        return;
    }
    channel->rtc = rtc;
    (void)h2_bk_webrtc_enqueue_event(
        channel->owner, H2_PAL_WEBRTC_EVENT_CHANNEL_MESSAGE, channel, 0, 0, 0,
        data, len, isBinary ? 0 : 1);
}

static void h2_bk_webrtc_on_data_channel(UINT64 customData, PRtcDataChannel rtc) {
    h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)(uintptr_t)customData;
    if (peer == NULL || rtc == NULL) {
        return;
    }
    h2_pal_webrtc_channel_t *channel = h2_bk_webrtc_find_channel_by_rtc(peer, rtc);
    if (channel == NULL) {
        channel = h2_bk_webrtc_find_channel_by_label(peer, rtc->name);
    }
    if (channel == NULL) {
        channel = h2_bk_webrtc_alloc_channel(peer, rtc->name, strlen(rtc->name));
        if (channel == NULL) {
            return;
        }
        h2_bk_webrtc_link_channel(peer, channel);
    }
    channel->rtc = rtc;
    channel->info.stream_id = (uint16_t)rtc->id;
    channel->info.has_stream_id = 1;
    (void)dataChannelOnOpen(rtc, (UINT64)(uintptr_t)channel, h2_bk_webrtc_on_open);
    (void)dataChannelOnMessage(rtc, (UINT64)(uintptr_t)channel, h2_bk_webrtc_on_message);
}

static void h2_bk_webrtc_on_connection_state(UINT64 customData, RTC_PEER_CONNECTION_STATE state) {
    h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)(uintptr_t)customData;
    if (peer == NULL) {
        return;
    }
    (void)h2_bk_webrtc_enqueue_event(
        peer, H2_PAL_WEBRTC_EVENT_PEER_STATE, NULL, 0,
        h2_bk_webrtc_map_state(state), 0, NULL, 0u, 0);
}

static void h2_bk_webrtc_on_ice_candidate(UINT64 customData, PCHAR candidate_json) {
    h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)(uintptr_t)customData;
    if (peer == NULL || candidate_json != NULL) {
        return;
    }
    peer->candidate_gathering_done = 1;
}

static int h2_bk_webrtc_ensure_initialized(void) {
    if (s_kvs_initialized) {
        return H2_PAL_OK;
    }
    int rc = h2_bk_webrtc_map_status(initKvsWebRtc());
    if (rc != H2_PAL_OK) {
        printf("H2_BK_WEBRTC_ERROR stage=init rc=%d\n", rc);
    }
    if (rc == H2_PAL_OK) {
        s_kvs_initialized = 1;
    }
    return rc;
}

static BOOL h2_bk_webrtc_filter_network_interface(UINT64 custom_data, PCHAR network_int) {
    (void)custom_data;
    BOOL use_interface = network_int != NULL &&
        STRNCMP(network_int, (PCHAR)"STA", ARRAY_SIZE("STA")) == 0;
    return use_interface;
}

static h2_pal_result_t h2_bk_webrtc_peer_create(
    void *user, h2_pal_webrtc_peer_t **out_peer) {
    (void)user;
    if (out_peer == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_peer = NULL;
    int init_rc = h2_bk_webrtc_ensure_initialized();
    if (init_rc != H2_PAL_OK) {
        return init_rc;
    }
    h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)h2_bk_webrtc_alloc(sizeof(*peer));
    if (peer == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    if (rtos_init_mutex(&peer->event_mutex) != kNoErr) {
        os_free(peer);
        return H2_PAL_ERR_NO_MEMORY;
    }
    RtcConfiguration config;
    memset(&config, 0, sizeof(config));
    config.iceTransportPolicy = ICE_TRANSPORT_POLICY_ALL;
    config.kvsRtcConfiguration.iceSetInterfaceFilterFunc =
        h2_bk_webrtc_filter_network_interface;
    config.kvsRtcConfiguration.iceLocalCandidateGatheringTimeout =
        12u * HUNDREDS_OF_NANOS_IN_A_SECOND;
    config.kvsRtcConfiguration.generateRSACertificate = FALSE;
    config.kvsRtcConfiguration.generatedCertificateBits = 256;
    config.kvsRtcConfiguration.disableSenderSideBandwidthEstimation = TRUE;
    STATUS status = createPeerConnection(&config, &peer->rtc);
    if (STATUS_FAILED(status)) {
        printf("H2_BK_WEBRTC_ERROR stage=create_peer status=0x%08lx\n", (unsigned long)status);
        (void)rtos_deinit_mutex(&peer->event_mutex);
        os_free(peer);
        return h2_bk_webrtc_map_status(status);
    }
    status = peerConnectionOnConnectionStateChange(peer->rtc, (UINT64)(uintptr_t)peer, h2_bk_webrtc_on_connection_state);
    if (STATUS_SUCCEEDED(status)) {
        status = peerConnectionOnDataChannel(peer->rtc, (UINT64)(uintptr_t)peer, h2_bk_webrtc_on_data_channel);
    }
    if (STATUS_SUCCEEDED(status)) {
        status = peerConnectionOnIceCandidate(peer->rtc, (UINT64)(uintptr_t)peer, h2_bk_webrtc_on_ice_candidate);
    }
    if (STATUS_FAILED(status)) {
        printf("H2_BK_WEBRTC_ERROR stage=peer_callbacks status=0x%08lx\n", (unsigned long)status);
        freePeerConnection(&peer->rtc);
        (void)rtos_deinit_mutex(&peer->event_mutex);
        os_free(peer);
        return h2_bk_webrtc_map_status(status);
    }
    *out_peer = peer;
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk_webrtc_peer_start_offer(h2_pal_webrtc_peer_t *peer) {
    if (peer == NULL || peer->rtc == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    PRtcSessionDescriptionInit offer =
        (PRtcSessionDescriptionInit)h2_bk_webrtc_alloc(sizeof(*offer));
    if (offer == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    offer->useTrickleIce = FALSE;
    peer->candidate_gathering_done = 0;
    STATUS status = setLocalDescription(peer->rtc, offer);
    if (STATUS_FAILED(status)) {
        printf("H2_BK_WEBRTC_ERROR stage=start_gathering status=0x%08lx\n", (unsigned long)status);
    }
    if (STATUS_SUCCEEDED(status)) {
        uint32_t waited_ms = 0u;
        while (!peer->candidate_gathering_done && waited_ms < 15000u) {
            (void)rtos_delay_milliseconds(20u);
            waited_ms += 20u;
        }
        if (!peer->candidate_gathering_done) {
            printf("H2_BK_WEBRTC_ERROR stage=ice_gathering_timeout waited_ms=%lu\n",
                (unsigned long)waited_ms);
            os_free(offer);
            return H2_PAL_ERR_TIMEOUT;
        }
        status = createOffer(peer->rtc, offer);
        if (STATUS_FAILED(status)) {
            printf("H2_BK_WEBRTC_ERROR stage=create_offer status=0x%08lx\n", (unsigned long)status);
        }
    }
    if (STATUS_FAILED(status)) {
        os_free(offer);
        return h2_bk_webrtc_map_status(status);
    }
    const size_t sdp_len = strlen(offer->sdp);
    const int event_rc = h2_bk_webrtc_enqueue_event(
        peer, H2_PAL_WEBRTC_EVENT_LOCAL_SDP, NULL, 0, 0,
        H2_PAL_WEBRTC_SDP_OFFER, offer->sdp, sdp_len, 0);
    os_free(offer);
    return event_rc;
}

static h2_pal_result_t h2_bk_webrtc_peer_set_remote_sdp(
    h2_pal_webrtc_peer_t *peer,
    h2_pal_webrtc_sdp_type_t type,
    h2_pal_webrtc_str_t sdp) {
    if (peer == NULL || peer->rtc == NULL || sdp.data == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (sdp.len > MAX_SESSION_DESCRIPTION_INIT_SDP_LEN) {
        return H2_PAL_ERR_NO_SPACE;
    }
    PRtcSessionDescriptionInit remote =
        (PRtcSessionDescriptionInit)h2_bk_webrtc_alloc(sizeof(*remote));
    if (remote == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    remote->type = h2_bk_webrtc_map_sdp_type(type);
    memcpy(remote->sdp, sdp.data, sdp.len);
    remote->sdp[sdp.len] = '\0';
    STATUS status = setRemoteDescription(peer->rtc, remote);
    if (STATUS_FAILED(status)) {
        printf("H2_BK_WEBRTC_ERROR stage=set_remote status=0x%08lx len=%lu\n",
            (unsigned long)status,
            (unsigned long)sdp.len);
    }
    int rc = h2_bk_webrtc_map_status(status);
    os_free(remote);
    return rc;
}

static h2_pal_result_t h2_bk_webrtc_peer_create_data_channel(
    h2_pal_webrtc_peer_t *peer,
    const h2_pal_webrtc_channel_config_t *config,
    h2_pal_webrtc_channel_t **out_channel) {
    if (peer == NULL || peer->rtc == NULL || config == NULL || out_channel == NULL ||
        config->label.data == NULL || config->label.len == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_webrtc_channel_t *channel =
        h2_bk_webrtc_alloc_channel(peer, config->label.data, config->label.len);
    if (channel == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    channel->info.ordered = config->ordered != 0;
    channel->info.reliable = config->reliable != 0;
    channel->info.stream_id = config->stream_id;
    channel->info.has_stream_id = config->has_stream_id != 0;
    RtcDataChannelInit init;
    memset(&init, 0, sizeof(init));
    init.ordered = config->ordered != 0 ? TRUE : FALSE;
    NULLABLE_SET_EMPTY(init.maxPacketLifeTime);
    if (config->reliable) {
        NULLABLE_SET_EMPTY(init.maxRetransmits);
    } else {
        NULLABLE_SET_VALUE(init.maxRetransmits, 0);
    }
    STATUS status = createDataChannel(peer->rtc, channel->label, &init, &channel->rtc);
    if (STATUS_FAILED(status)) {
        printf("H2_BK_WEBRTC_ERROR stage=create_channel status=0x%08lx label=%s\n",
            (unsigned long)status,
            channel->label);
        os_free(channel);
        return h2_bk_webrtc_map_status(status);
    }
    channel->info.stream_id = (uint16_t)channel->rtc->id;
    channel->info.has_stream_id = 1;
    status = dataChannelOnOpen(channel->rtc, (UINT64)(uintptr_t)channel, h2_bk_webrtc_on_open);
    if (STATUS_SUCCEEDED(status)) {
        status = dataChannelOnMessage(channel->rtc, (UINT64)(uintptr_t)channel, h2_bk_webrtc_on_message);
    }
    if (STATUS_FAILED(status)) {
        printf("H2_BK_WEBRTC_ERROR stage=channel_callbacks status=0x%08lx label=%s\n",
            (unsigned long)status,
            channel->label);
        os_free(channel);
        return h2_bk_webrtc_map_status(status);
    }
    h2_bk_webrtc_link_channel(peer, channel);
    *out_channel = channel;
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk_webrtc_peer_poll(
    h2_pal_webrtc_peer_t *peer, int timeout_ms,
    h2_pal_webrtc_event_t *out_event) {
    if (peer == NULL || out_event == NULL || timeout_ms < 0) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    uint32_t remaining_ms = (uint32_t)timeout_ms;
    for (;;) {
        const int rc = h2_bk_webrtc_dequeue_event(peer, out_event);
        if (rc != H2_PAL_ERR_WOULD_BLOCK) {
            return rc;
        }
        if (remaining_ms == 0u) {
            return timeout_ms == 0 ? H2_PAL_ERR_WOULD_BLOCK
                                   : H2_PAL_ERR_TIMEOUT;
        }
        (void)rtos_delay_milliseconds(1u);
        --remaining_ms;
    }
}

static h2_pal_result_t h2_bk_webrtc_peer_send_opus(
    h2_pal_webrtc_peer_t *peer,
    const uint8_t *opus,
    size_t opus_len) {
    (void)peer;
    (void)opus;
    (void)opus_len;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t h2_bk_webrtc_channel_send(
    h2_pal_webrtc_channel_t *channel,
    const uint8_t *data,
    size_t len,
    int is_text) {
    if (channel == NULL || channel->rtc == NULL || data == NULL || len > UINT32_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    STATUS status = dataChannelSend(channel->rtc, is_text ? FALSE : TRUE, (PBYTE)data, (UINT32)len);
    if (STATUS_FAILED(status)) {
        printf("H2_BK_WEBRTC_ERROR stage=channel_send status=0x%08lx label=%s len=%lu\n",
            (unsigned long)status,
            channel->label,
            (unsigned long)len);
    }
    return h2_bk_webrtc_map_status(status);
}

static void h2_bk_webrtc_channel_close(h2_pal_webrtc_channel_t *channel) {
    if (channel == NULL || channel->owner == NULL) {
        return;
    }
    (void)h2_bk_webrtc_enqueue_event(
        channel->owner, H2_PAL_WEBRTC_EVENT_CHANNEL_STATE, channel,
        H2_PAL_WEBRTC_CHANNEL_CLOSED, 0, 0, NULL, 0u, 0);
}

static void h2_bk_webrtc_peer_close(h2_pal_webrtc_peer_t *peer) {
    if (peer == NULL) {
        return;
    }
    if (peer->rtc != NULL) {
        (void)closePeerConnection(peer->rtc);
        (void)freePeerConnection(&peer->rtc);
    }
    h2_pal_webrtc_channel_t *channel = peer->channels;
    while (channel != NULL) {
        h2_pal_webrtc_channel_t *next = channel->next;
        os_free(channel);
        channel = next;
    }
    if (rtos_lock_mutex(&peer->event_mutex) == kNoErr) {
        h2_bk_webrtc_event_t *event = peer->event_head;
        peer->event_head = NULL;
        peer->event_tail = NULL;
        peer->event_count = 0u;
        (void)rtos_unlock_mutex(&peer->event_mutex);
        while (event != NULL) {
            h2_bk_webrtc_event_t *next = event->next;
            h2_pal_webrtc_event_t public_event = event->event;
            public_event._private = event;
            public_event._release = h2_bk_webrtc_event_release;
            h2_bk_webrtc_event_release(&public_event);
            event = next;
        }
    }
    (void)rtos_deinit_mutex(&peer->event_mutex);
    os_free(peer);
}

static const h2_pal_webrtc_vtable_t s_h2_bk_webrtc_vtable = {
    .peer_create = h2_bk_webrtc_peer_create,
    .peer_start_offer = h2_bk_webrtc_peer_start_offer,
    .peer_set_remote_sdp = h2_bk_webrtc_peer_set_remote_sdp,
    .peer_create_data_channel = h2_bk_webrtc_peer_create_data_channel,
    .peer_poll = h2_bk_webrtc_peer_poll,
    .peer_send_opus = h2_bk_webrtc_peer_send_opus,
    .channel_send = h2_bk_webrtc_channel_send,
    .channel_close = h2_bk_webrtc_channel_close,
    .peer_close = h2_bk_webrtc_peer_close,
};

static const h2_pal_webrtc_api_t s_h2_bk_webrtc_api = {
    .user = NULL,
    .vtable = &s_h2_bk_webrtc_vtable,
};

const h2_pal_webrtc_api_t *h2_bk_platform_webrtc_api(void) {
    return &s_h2_bk_webrtc_api;
}

#else

const h2_pal_webrtc_api_t *h2_bk_platform_webrtc_api(void) {
    return NULL;
}

#endif
