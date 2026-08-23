#include "h2_esp_audio_decoder.h"

#include "esp_audio_dec.h"
#include "esp_log.h"
#include "impl/esp_aac_dec.h"

#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

#define H2_ESP_AAC_SAMPLES_PER_CHANNEL 1024u

static const char *TAG = "h2_esp_audio_decoder";

typedef struct h2_esp_audio_decoder_session {
    h2_pal_mem_api_t allocator;
    esp_audio_dec_handle_t decoder;
    uint8_t *pcm;
    size_t pcm_capacity;
    h2_audio_decoder_frame_info_t frame_info;
    uint32_t sample_rate_hz;
    uint8_t channels;
    int configured;
    int ready;
    int acquired;
    int eos;
} h2_esp_audio_decoder_session_t;

struct h2_pal_audio_decoder_frame {
    h2_esp_audio_decoder_session_t *owner;
};

struct h2_pal_audio_decoder_session {
    h2_esp_audio_decoder_session_t state;
    struct h2_pal_audio_decoder_frame frame;
};

static h2_pal_result_t map_error(esp_audio_err_t error) {
    switch (error) {
    case ESP_AUDIO_ERR_OK:
        return H2_PAL_OK;
    case ESP_AUDIO_ERR_MEM_LACK:
    case ESP_AUDIO_ERR_BUFF_NOT_ENOUGH:
        return H2_PAL_ERR_NO_MEMORY;
    case ESP_AUDIO_ERR_NOT_SUPPORT:
        return H2_PAL_ERR_UNSUPPORTED;
    case ESP_AUDIO_ERR_INVALID_PARAMETER:
        return H2_PAL_ERR_INVALID_ARG;
    case ESP_AUDIO_ERR_DATA_LACK:
    case ESP_AUDIO_ERR_HEADER_PARSE:
        return H2_PAL_ERR_FORMAT;
    default:
        return H2_PAL_ERR_IO;
    }
}

static int parse_audio_specific_config(
    const uint8_t *config,
    size_t size,
    uint32_t *out_sample_rate_hz,
    uint8_t *out_channels) {
    static const uint32_t sample_rates[] = {
        96000u, 88200u, 64000u, 48000u, 44100u, 32000u, 24000u,
        22050u, 16000u, 12000u, 11025u, 8000u, 7350u,
    };
    if (config == NULL || size < 2u) {
        return 0;
    }
    const uint16_t bits = ((uint16_t)config[0] << 8u) | config[1];
    const uint8_t audio_object_type = (uint8_t)((bits >> 11u) & 0x1fu);
    const uint8_t sample_rate_index = (uint8_t)((bits >> 7u) & 0x0fu);
    const uint8_t channels = (uint8_t)((bits >> 3u) & 0x0fu);
    if (audio_object_type != 2u ||
        sample_rate_index >= sizeof(sample_rates) / sizeof(sample_rates[0]) ||
        channels == 0u || channels > 2u) {
        return 0;
    }
    *out_sample_rate_hz = sample_rates[sample_rate_index];
    *out_channels = channels;
    return 1;
}

static void release_configuration(h2_esp_audio_decoder_session_t *session) {
    if (session->decoder != NULL) {
        (void)esp_aac_dec_close(session->decoder);
        session->decoder = NULL;
    }
    h2_pal_mem_free(&session->allocator, session->pcm);
    session->pcm = NULL;
    session->pcm_capacity = 0u;
    session->configured = 0;
    session->ready = 0;
    session->eos = 0;
}

static h2_pal_result_t decoder_open(
    void *user,
    const h2_audio_decoder_config_t *config,
    h2_pal_audio_decoder_session_t **out_session) {
    (void)user;
    if (config->preferred_format != H2_AUDIO_SAMPLE_S16LE) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    h2_pal_audio_decoder_session_t *session =
        h2_pal_mem_alloc(config->pcm_allocator, sizeof(*session));
    if (session == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(session, 0, sizeof(*session));
    session->state.allocator = *config->pcm_allocator;
    *out_session = session;
    return H2_PAL_OK;
}

static h2_pal_result_t decoder_configure(
    void *user,
    h2_pal_audio_decoder_session_t *opaque,
    const h2_audio_decoder_stream_config_t *config) {
    (void)user;
    h2_esp_audio_decoder_session_t *session = &opaque->state;
    if (session->configured || session->acquired) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (config->codec != H2_AUDIO_CODEC_AAC_LC ||
        config->bitstream_format != H2_AUDIO_BITSTREAM_AAC_RAW ||
        config->channels > 2u) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    uint32_t asc_sample_rate_hz = 0u;
    uint8_t asc_channels = 0u;
    if (!parse_audio_specific_config(
            config->codec_config,
            config->codec_config_size,
            &asc_sample_rate_hz,
            &asc_channels) ||
        asc_sample_rate_hz != config->sample_rate_hz ||
        asc_channels != config->channels) {
        return H2_PAL_ERR_FORMAT;
    }
    const size_t pcm_capacity =
        H2_ESP_AAC_SAMPLES_PER_CHANNEL * config->channels * sizeof(int16_t);
    uint8_t *pcm = h2_pal_mem_alloc(&session->allocator, pcm_capacity);
    if (pcm == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    esp_aac_dec_cfg_t aac_config = ESP_AAC_DEC_CONFIG_DEFAULT();
    aac_config.sample_rate = (int32_t)config->sample_rate_hz;
    aac_config.channel = config->channels;
    aac_config.bits_per_sample = 16u;
    aac_config.no_adts_header = true;
    aac_config.aac_plus_enable = false;
    esp_audio_dec_handle_t decoder = NULL;
    const esp_audio_err_t open_result =
        esp_aac_dec_open(&aac_config, sizeof(aac_config), &decoder);
    if (open_result != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(
            TAG,
            "AAC open failed: code=%d rate=%" PRIu32 " channels=%u",
            open_result,
            config->sample_rate_hz,
            config->channels);
        h2_pal_mem_free(&session->allocator, pcm);
        return map_error(open_result);
    }
    session->decoder = decoder;
    session->pcm = pcm;
    session->pcm_capacity = pcm_capacity;
    session->sample_rate_hz = config->sample_rate_hz;
    session->channels = config->channels;
    session->configured = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t decoder_submit(
    void *user,
    h2_pal_audio_decoder_session_t *opaque,
    const h2_audio_decoder_packet_t *packet) {
    (void)user;
    h2_esp_audio_decoder_session_t *session = &opaque->state;
    if (!session->configured || session->eos) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (packet->flags != 0u) {
        session->eos = 1;
        return H2_PAL_OK;
    }
    if (session->ready || session->acquired) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    if (packet->size > UINT32_MAX) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    esp_audio_dec_in_raw_t input = {0};
    esp_audio_dec_out_frame_t output = {0};
    esp_audio_dec_info_t info = {0};
    esp_audio_err_t decode_result = ESP_AUDIO_ERR_OK;
    for (unsigned int attempt = 0u; attempt < 2u; ++attempt) {
        input = (esp_audio_dec_in_raw_t){
            .buffer = (uint8_t *)(uintptr_t)packet->data,
            .len = (uint32_t)packet->size,
        };
        output = (esp_audio_dec_out_frame_t){
            .buffer = session->pcm,
            .len = (uint32_t)session->pcm_capacity,
        };
        memset(&info, 0, sizeof(info));
        decode_result =
            esp_aac_dec_decode(session->decoder, &input, &output, &info);
        if (decode_result != ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            break;
        }
        if (input.consumed != 0u ||
            output.needed_size <= session->pcm_capacity) {
            break;
        }
        uint8_t *larger_pcm =
            h2_pal_mem_alloc(&session->allocator, output.needed_size);
        if (larger_pcm == NULL) {
            return H2_PAL_ERR_NO_MEMORY;
        }
        h2_pal_mem_free(&session->allocator, session->pcm);
        session->pcm = larger_pcm;
        session->pcm_capacity = output.needed_size;
    }
    if (decode_result != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(
            TAG,
            "AAC decode failed: code=%d input=%" PRIu32
            " consumed=%" PRIu32 " output=%" PRIu32
            " needed=%" PRIu32,
            decode_result,
            input.len,
            input.consumed,
            output.decoded_size,
            output.needed_size);
        return map_error(decode_result);
    }
    if (input.consumed != packet->size || output.decoded_size == 0u ||
        output.decoded_size > session->pcm_capacity ||
        info.sample_rate != session->sample_rate_hz ||
        info.channel != session->channels || info.bits_per_sample != 16u ||
        output.decoded_size %
            ((size_t)session->channels * sizeof(int16_t)) != 0u) {
        ESP_LOGE(
            TAG,
            "AAC frame mismatch: input=%" PRIu32 "/%zu output=%" PRIu32
            "/%zu rate=%" PRIu32 "/%" PRIu32
            " channels=%u/%u bits=%u",
            input.consumed,
            packet->size,
            output.decoded_size,
            session->pcm_capacity,
            info.sample_rate,
            session->sample_rate_hz,
            info.channel,
            session->channels,
            info.bits_per_sample);
        return H2_PAL_ERR_FORMAT;
    }
    session->frame_info = (h2_audio_decoder_frame_info_t){
        .data = session->pcm,
        .bytes = output.decoded_size,
        .sample_rate_hz = info.sample_rate,
        .samples_per_channel =
            output.decoded_size / (session->channels * sizeof(int16_t)),
        .channels = info.channel,
        .sample_format = H2_AUDIO_SAMPLE_S16LE,
        .pts_us = packet->pts_us,
        .duration_us = packet->duration_us,
    };
    session->ready = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t decoder_acquire(
    void *user,
    h2_pal_audio_decoder_session_t *opaque,
    uint32_t timeout_ms,
    h2_pal_audio_decoder_frame_t **out_frame) {
    (void)user;
    (void)timeout_ms;
    h2_esp_audio_decoder_session_t *session = &opaque->state;
    if (session->acquired) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    if (!session->ready) {
        return session->eos ? H2_PAL_EXIT : H2_PAL_ERR_WOULD_BLOCK;
    }
    opaque->frame.owner = session;
    session->acquired = 1;
    *out_frame = &opaque->frame;
    return H2_PAL_OK;
}

static h2_pal_result_t decoder_get_info(
    void *user,
    h2_pal_audio_decoder_session_t *opaque,
    h2_pal_audio_decoder_frame_t *frame,
    h2_audio_decoder_frame_info_t *out_info) {
    (void)user;
    h2_esp_audio_decoder_session_t *session = &opaque->state;
    if (!session->acquired || frame != &opaque->frame ||
        frame->owner != session) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    *out_info = session->frame_info;
    return H2_PAL_OK;
}

static h2_pal_result_t decoder_release(
    void *user,
    h2_pal_audio_decoder_session_t *opaque,
    h2_pal_audio_decoder_frame_t *frame) {
    (void)user;
    h2_esp_audio_decoder_session_t *session = &opaque->state;
    if (!session->acquired || frame != &opaque->frame ||
        frame->owner != session) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    session->acquired = 0;
    session->ready = 0;
    opaque->frame.owner = NULL;
    return H2_PAL_OK;
}

static h2_pal_result_t decoder_reset(
    void *user,
    h2_pal_audio_decoder_session_t *opaque) {
    (void)user;
    h2_esp_audio_decoder_session_t *session = &opaque->state;
    if (session->acquired) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    release_configuration(session);
    return H2_PAL_OK;
}

static h2_pal_result_t decoder_close(
    void *user,
    h2_pal_audio_decoder_session_t *opaque) {
    (void)user;
    h2_esp_audio_decoder_session_t *session = &opaque->state;
    if (session->acquired) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    const h2_pal_mem_api_t allocator = session->allocator;
    release_configuration(session);
    h2_pal_mem_free(&allocator, opaque);
    return H2_PAL_OK;
}

static const h2_pal_audio_decoder_vtable_t s_vtable = {
    .open = decoder_open,
    .configure = decoder_configure,
    .submit_packet = decoder_submit,
    .acquire_frame = decoder_acquire,
    .frame_get_info = decoder_get_info,
    .release_frame = decoder_release,
    .reset = decoder_reset,
    .close = decoder_close,
};

static const h2_pal_audio_decoder_api_t s_api = {
    .user = NULL,
    .vtable = &s_vtable,
};

const h2_pal_audio_decoder_api_t *h2_esp_audio_decoder_api(void) {
    return &s_api;
}
