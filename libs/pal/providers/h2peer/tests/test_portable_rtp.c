#include "rtp.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

typedef struct test_send_state {
    int block_once;
    uint8_t packet[32];
    size_t packet_len;
} test_send_state_t;

typedef struct test_receive_state {
    size_t audio_count;
    size_t video_count;
    uint8_t payload[8];
    size_t payload_len;
} test_receive_state_t;

static int test_send(uint8_t *packet, size_t len, void *user) {
    test_send_state_t *state = (test_send_state_t *)user;
    assert(len <= sizeof(state->packet));
    memcpy(state->packet, packet, len);
    state->packet_len = len;
    if (state->block_once) {
        state->block_once = 0;
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    return (int)len;
}

static void test_receive_audio(uint8_t *payload, size_t len, void *user) {
    test_receive_state_t *state = (test_receive_state_t *)user;
    assert(len <= sizeof(state->payload));
    memcpy(state->payload, payload, len);
    state->payload_len = len;
    state->audio_count++;
}

static void test_receive_video(uint8_t *payload, size_t len, void *user) {
    test_receive_state_t *state = (test_receive_state_t *)user;
    (void)payload;
    (void)len;
    state->video_count++;
}

int main(void) {
    test_send_state_t state = {.block_once = 1};
    RtpEncoder encoder;
    rtp_encoder_init(&encoder, CODEC_OPUS, test_send, &state);
    const uint8_t opus[] = {0xf8u, 0x55u};

    assert(rtp_encoder_encode(&encoder, opus, sizeof(opus)) ==
           H2_PAL_ERR_WOULD_BLOCK);
    assert(encoder.seq_number == 0u && encoder.timestamp == 0u);
    assert(state.packet_len == 14u && state.packet[0] == 0x80u &&
           state.packet[1] == 0x6fu && state.packet[12] == 0xf8u &&
           state.packet[13] == 0x55u);

    assert(rtp_encoder_encode(&encoder, opus, sizeof(opus)) == 0);
    assert(encoder.seq_number == 1u && encoder.timestamp != 0u);
    assert(state.packet[1] == 0x6fu && state.packet[2] == 0u &&
           state.packet[3] == 0u && state.packet[4] == 0u &&
           state.packet[5] == 0u && state.packet[6] == 0u &&
           state.packet[7] == 0u);

    RtpDecoder audio_decoder;
    RtpDecoder video_decoder;
    test_receive_state_t receive = {0};
    rtp_decoder_init(&audio_decoder, CODEC_OPUS, test_receive_audio, &receive);
    rtp_decoder_init(&video_decoder, CODEC_H264, test_receive_video, &receive);
    uint8_t remote_packet[] = {
        0x80u, 0x6fu, 0x12u, 0x34u, 0u, 0u, 0u, 1u,
        0xdeu, 0xadu, 0xbeu, 0xefu, 0xf8u, 0x55u,
    };
    assert(rtp_get_ssrc(remote_packet) == UINT32_C(0xdeadbeef));
    assert(rtp_decoders_decode(&audio_decoder, &video_decoder, remote_packet,
                               sizeof(remote_packet)) ==
           (int)sizeof(remote_packet));
    assert(receive.audio_count == 1u && receive.video_count == 0u);
    assert(receive.payload_len == sizeof(opus));
    assert(memcmp(receive.payload, opus, sizeof(opus)) == 0);

    remote_packet[8] = 0x01u;
    remote_packet[9] = 0x02u;
    remote_packet[10] = 0x03u;
    remote_packet[11] = 0x04u;
    assert(rtp_get_ssrc(remote_packet) == UINT32_C(0x01020304));
    assert(rtp_decoders_decode(&audio_decoder, &video_decoder, remote_packet,
                               sizeof(remote_packet)) ==
           (int)sizeof(remote_packet));
    assert(receive.audio_count == 2u && receive.video_count == 0u);

    remote_packet[1] = 95u;
    assert(rtp_decoders_decode(&audio_decoder, &video_decoder, remote_packet,
                               sizeof(remote_packet)) == -1);
    assert(receive.audio_count == 2u && receive.video_count == 0u);

    remote_packet[1] = 0x6fu;
    rtp_decoder_init(&video_decoder, CODEC_OPUS, test_receive_video, &receive);
    assert(rtp_decoders_decode(&audio_decoder, &video_decoder, remote_packet,
                               sizeof(remote_packet)) ==
           (int)sizeof(remote_packet));
    assert(receive.audio_count == 3u && receive.video_count == 0u);
    return 0;
}
