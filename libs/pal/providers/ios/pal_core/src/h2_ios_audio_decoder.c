#include "h2_ios_platform.h"
#include <AudioToolbox/AudioToolbox.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

enum { H2_IOS_AAC_FRAME_SAMPLES = 1024, H2_IOS_AAC_TIMESTAMPS = 32,
       H2_IOS_AAC_NEED_INPUT = -1234567 };

typedef struct owned_packet {
    AudioStreamPacketDescription description;
    int64_t pts_us;
    uint8_t data[];
} owned_packet_t;

struct h2_pal_audio_decoder_frame {
    struct h2_pal_audio_decoder_session *owner;
};

struct h2_pal_audio_decoder_session {
    h2_pal_mem_api_t allocator;
    AudioConverterRef codec;
    owned_packet_t *pending;
    owned_packet_t *last_input;
    int16_t *pcm;
    size_t pcm_bytes;
    uint32_t rate;
    uint8_t channels;
    int acquired;
    int eos_submitted;
    int eos_reached;
    int configured;
    int64_t timestamps[H2_IOS_AAC_TIMESTAMPS];
    unsigned timestamp_head;
    unsigned timestamp_count;
    h2_audio_decoder_frame_info_t info;
    struct h2_pal_audio_decoder_frame frame;
};

static h2_pal_result_t native_error(OSStatus status) {
    if (status == noErr) return H2_PAL_OK;
    if (status == kAudioConverterErr_FormatNotSupported ||
        status == kAudioConverterErr_OperationNotSupported)
        return H2_PAL_ERR_UNSUPPORTED;
    if (status == kAudioConverterErr_InvalidInputSize ||
        status == kAudioConverterErr_RequiresPacketDescriptionsError)
        return H2_PAL_ERR_FORMAT;
    return H2_PAL_ERR_IO;
}

static size_t descriptor_length_bytes(size_t length) {
    size_t count = 1u;
    while ((length >>= 7u) != 0u) ++count;
    return count;
}
static uint8_t *write_descriptor_length(uint8_t *p, size_t length) {
    size_t count = descriptor_length_bytes(length);
    for (size_t i = count; i > 0u; --i) {
        unsigned byte = (unsigned)((length >> ((i - 1u) * 7u)) & 0x7fu);
        if (i > 1u) byte |= 0x80u;
        *p++ = (uint8_t)byte;
    }
    return p;
}

/* CoreAudio consumes an MPEG-4 ES descriptor cookie, while PAL carries ASC. */
static h2_pal_result_t set_cookie(h2_pal_audio_decoder_session_t *s,
                                   AudioConverterRef codec, const void *asc,
                                   size_t asc_size) {
    if (asc_size > 0x0fffff00u) return H2_PAL_ERR_INVALID_ARG;
    const size_t specific = 1u + descriptor_length_bytes(asc_size) + asc_size;
    const size_t decoder = 13u + specific;
    const size_t elementary = 3u + 1u + descriptor_length_bytes(decoder) + decoder + 3u;
    const size_t total = 1u + descriptor_length_bytes(elementary) + elementary;
    uint8_t *cookie = h2_pal_mem_alloc(&s->allocator, total);
    if (!cookie) return H2_PAL_ERR_NO_MEMORY;
    uint8_t *p = cookie;
    *p++ = 3u; p = write_descriptor_length(p, elementary);
    *p++ = 0u; *p++ = 1u; *p++ = 0u;
    *p++ = 4u; p = write_descriptor_length(p, decoder);
    *p++ = 0x40u; *p++ = 0x15u;
    memset(p, 0, 11u); p += 11u;
    *p++ = 5u; p = write_descriptor_length(p, asc_size);
    memcpy(p, asc, asc_size); p += asc_size;
    *p++ = 6u; *p++ = 1u; *p++ = 2u;
    h2_pal_result_t rc = (size_t)(p - cookie) == total
        ? native_error(AudioConverterSetProperty(codec,
            kAudioConverterDecompressionMagicCookie, (UInt32)total, cookie))
        : H2_PAL_ERR_INVALID_STATE;
    h2_pal_mem_free(&s->allocator, cookie);
    return rc;
}

static OSStatus input_callback(AudioConverterRef converter, UInt32 *packets,
                                AudioBufferList *data,
                                AudioStreamPacketDescription **descriptions,
                                void *user) {
    (void)converter;
    h2_pal_audio_decoder_session_t *s = user;
    /* CoreAudio may retain the previous input until this callback is invoked again. */
    h2_pal_mem_free(&s->allocator, s->last_input);
    s->last_input = NULL;
    if (!s->pending) {
        *packets = 0u;
        data->mNumberBuffers = 1u;
        data->mBuffers[0] = (AudioBuffer){.mNumberChannels = s->channels};
        if (descriptions) *descriptions = NULL;
        return s->eos_submitted ? noErr : H2_IOS_AAC_NEED_INPUT;
    }
    if (s->timestamp_count == H2_IOS_AAC_TIMESTAMPS) {
        *packets = 0u;
        return kAudioConverterErr_UnspecifiedError;
    }
    owned_packet_t *packet = s->pending;
    s->pending = NULL;
    s->last_input = packet;
    unsigned tail = (s->timestamp_head + s->timestamp_count) % H2_IOS_AAC_TIMESTAMPS;
    s->timestamps[tail] = packet->pts_us;
    ++s->timestamp_count;
    *packets = 1u;
    data->mNumberBuffers = 1u;
    data->mBuffers[0] = (AudioBuffer){
        .mNumberChannels = s->channels,
        .mDataByteSize = packet->description.mDataByteSize,
        .mData = packet->data};
    if (descriptions) *descriptions = &packet->description;
    return noErr;
}

static h2_pal_result_t drop_configuration(h2_pal_audio_decoder_session_t *s) {
    if (s->codec) {
        h2_pal_result_t rc = native_error(AudioConverterDispose(s->codec));
        if (rc != H2_PAL_OK) return rc;
        s->codec = NULL;
    }
    h2_pal_mem_free(&s->allocator, s->pending);
    h2_pal_mem_free(&s->allocator, s->last_input);
    h2_pal_mem_free(&s->allocator, s->pcm);
    s->pending = s->last_input = NULL;
    s->pcm = NULL;
    s->pcm_bytes = 0u;
    s->configured = s->eos_submitted = s->eos_reached = 0;
    s->timestamp_head = s->timestamp_count = 0u;
    memset(&s->info, 0, sizeof(s->info));
    return H2_PAL_OK;
}

static h2_pal_result_t decoder_open(void *user,
    const h2_audio_decoder_config_t *config,
    h2_pal_audio_decoder_session_t **out_session) {
    (void)user;
    if (out_session) *out_session = NULL;
    if (!config || !out_session || !h2_audio_decoder_allocator_is_valid(config->pcm_allocator))
        return H2_PAL_ERR_INVALID_ARG;
    if (config->preferred_format != H2_AUDIO_SAMPLE_S16LE)
        return H2_PAL_ERR_UNSUPPORTED;
    h2_pal_audio_decoder_session_t *s = h2_pal_mem_alloc(config->pcm_allocator, sizeof(*s));
    if (!s) return H2_PAL_ERR_NO_MEMORY;
    memset(s, 0, sizeof(*s));
    s->allocator = *config->pcm_allocator;
    s->frame.owner = s;
    *out_session = s;
    return H2_PAL_OK;
}

static h2_pal_result_t decoder_configure(void *user,
    h2_pal_audio_decoder_session_t *s,
    const h2_audio_decoder_stream_config_t *config) {
    (void)user;
    if (!s || !config) return H2_PAL_ERR_INVALID_ARG;
    if (s->configured || s->acquired) return H2_PAL_ERR_INVALID_STATE;
    if (config->codec != H2_AUDIO_CODEC_AAC_LC ||
        config->bitstream_format != H2_AUDIO_BITSTREAM_AAC_RAW)
        return H2_PAL_ERR_UNSUPPORTED;
    if (!config->codec_config || config->codec_config_size < 2u)
        return H2_PAL_ERR_FORMAT;
    if (config->codec_config_size > UINT32_MAX) return H2_PAL_ERR_INVALID_ARG;
    static const uint32_t rates[] = {
        96000u, 88200u, 64000u, 48000u, 44100u, 32000u, 24000u,
        22050u, 16000u, 12000u, 11025u, 8000u, 7350u};
    const uint8_t *asc = config->codec_config;
    const unsigned bits = ((unsigned)asc[0] << 8u) | asc[1];
    const unsigned object_type = bits >> 11u;
    const unsigned rate_index = (bits >> 7u) & 15u;
    const unsigned channels = (bits >> 3u) & 15u;
    if (object_type != 2u || rate_index >= sizeof(rates) / sizeof(rates[0]) ||
        channels == 0u || channels > 2u || (bits & 4u))
        return H2_PAL_ERR_UNSUPPORTED;
    if (rates[rate_index] != config->sample_rate_hz || channels != config->channels)
        return H2_PAL_ERR_FORMAT;
    AudioStreamBasicDescription input = {
        .mSampleRate = config->sample_rate_hz,
        .mFormatID = kAudioFormatMPEG4AAC,
        .mFramesPerPacket = H2_IOS_AAC_FRAME_SAMPLES,
        .mChannelsPerFrame = config->channels};
    AudioStreamBasicDescription output = {
        .mSampleRate = config->sample_rate_hz,
        .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
        .mBytesPerPacket = config->channels * sizeof(int16_t), .mFramesPerPacket = 1u,
        .mBytesPerFrame = config->channels * sizeof(int16_t),
        .mChannelsPerFrame = config->channels, .mBitsPerChannel = 16u};
    AudioConverterRef codec = NULL;
    h2_pal_result_t rc = native_error(AudioConverterNew(&input, &output, &codec));
    if (rc != H2_PAL_OK) return rc;
    rc = set_cookie(s, codec, config->codec_config, config->codec_config_size);
    if (rc != H2_PAL_OK) { (void)AudioConverterDispose(codec); return rc; }
    s->codec = codec;
    s->rate = config->sample_rate_hz;
    s->channels = config->channels;
    s->configured = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t decoder_submit(void *user,
    h2_pal_audio_decoder_session_t *s, const h2_audio_decoder_packet_t *packet) {
    (void)user;
    if (!s || !packet) return H2_PAL_ERR_INVALID_ARG;
    if (!s->configured || s->eos_submitted) return H2_PAL_ERR_INVALID_STATE;
    if (s->pending || s->acquired) return H2_PAL_ERR_WOULD_BLOCK;
    if (packet->flags & H2_AUDIO_DECODER_PACKET_END_OF_STREAM) {
        s->eos_submitted = 1;
        return H2_PAL_OK;
    }
    if (!packet->data || !packet->size || packet->size > UINT32_MAX ||
        packet->size > SIZE_MAX - sizeof(owned_packet_t))
        return H2_PAL_ERR_INVALID_ARG;
    owned_packet_t *owned = h2_pal_mem_alloc(&s->allocator, sizeof(*owned) + packet->size);
    if (!owned) return H2_PAL_ERR_NO_MEMORY;
    owned->description = (AudioStreamPacketDescription){
        .mStartOffset = 0, .mVariableFramesInPacket = H2_IOS_AAC_FRAME_SAMPLES,
        .mDataByteSize = (UInt32)packet->size};
    owned->pts_us = packet->pts_us;
    memcpy(owned->data, packet->data, packet->size);
    s->pending = owned;
    return H2_PAL_OK;
}

static h2_pal_result_t decoder_acquire(void *user,
    h2_pal_audio_decoder_session_t *s, uint32_t timeout_ms,
    h2_pal_audio_decoder_frame_t **out_frame) {
    (void)user;
    if (out_frame) *out_frame = NULL;
    if (!s || !out_frame) return H2_PAL_ERR_INVALID_ARG;
    if (!s->configured) return H2_PAL_ERR_INVALID_STATE;
    if (s->acquired) return H2_PAL_ERR_WOULD_BLOCK;
    if (s->eos_reached) return H2_PAL_EXIT;
    if (!s->pcm) {
        size_t size = H2_IOS_AAC_FRAME_SAMPLES * s->channels * sizeof(int16_t);
        s->pcm = h2_pal_mem_alloc(&s->allocator, size);
        if (!s->pcm) return H2_PAL_ERR_NO_MEMORY;
        s->pcm_bytes = size;
    }
    UInt32 samples = H2_IOS_AAC_FRAME_SAMPLES;
    AudioBufferList output = {.mNumberBuffers = 1u, .mBuffers = {{
        .mNumberChannels = s->channels, .mDataByteSize = (UInt32)s->pcm_bytes,
        .mData = s->pcm}}};
    OSStatus native = AudioConverterFillComplexBuffer(s->codec, input_callback, s,
                                                      &samples, &output, NULL);
    if (native != noErr && native != H2_IOS_AAC_NEED_INPUT) return native_error(native);
    if (samples == 0u) {
        if (native == noErr && s->eos_submitted && !s->pending) {
            s->eos_reached = 1;
            return H2_PAL_EXIT;
        }
        return timeout_ms == 0u ? H2_PAL_ERR_WOULD_BLOCK : H2_PAL_ERR_TIMEOUT;
    }
    if (!s->timestamp_count || output.mBuffers[0].mDataByteSize !=
        samples * s->channels * sizeof(int16_t)) return H2_PAL_ERR_FORMAT;
    int64_t pts = s->timestamps[s->timestamp_head];
    s->timestamp_head = (s->timestamp_head + 1u) % H2_IOS_AAC_TIMESTAMPS;
    --s->timestamp_count;
    s->info = (h2_audio_decoder_frame_info_t){
        .data = s->pcm, .bytes = output.mBuffers[0].mDataByteSize,
        .sample_rate_hz = s->rate, .samples_per_channel = samples,
        .channels = s->channels, .sample_format = H2_AUDIO_SAMPLE_S16LE,
        .pts_us = pts, .duration_us = (int64_t)samples * 1000000 / s->rate};
    s->acquired = 1;
    *out_frame = &s->frame;
    return H2_PAL_OK;
}

static h2_pal_result_t decoder_info(void *user, h2_pal_audio_decoder_session_t *s,
    h2_pal_audio_decoder_frame_t *frame, h2_audio_decoder_frame_info_t *out_info) {
    (void)user;
    if (!s || !out_info || frame != &s->frame) return H2_PAL_ERR_INVALID_ARG;
    if (!s->acquired) return H2_PAL_ERR_INVALID_STATE;
    *out_info = s->info;
    return H2_PAL_OK;
}
static h2_pal_result_t decoder_release(void *user, h2_pal_audio_decoder_session_t *s,
                                      h2_pal_audio_decoder_frame_t *frame) {
    (void)user;
    if (!s || frame != &s->frame) return H2_PAL_ERR_INVALID_ARG;
    if (!s->acquired) return H2_PAL_ERR_INVALID_STATE;
    s->acquired = 0;
    memset(&s->info, 0, sizeof(s->info));
    return H2_PAL_OK;
}
static h2_pal_result_t decoder_reset(void *user, h2_pal_audio_decoder_session_t *s) {
    (void)user;
    if (!s) return H2_PAL_ERR_INVALID_ARG;
    if (s->acquired) return H2_PAL_ERR_INVALID_STATE;
    return drop_configuration(s);
}
static h2_pal_result_t decoder_close(void *user, h2_pal_audio_decoder_session_t *s) {
    (void)user;
    if (!s) return H2_PAL_ERR_INVALID_ARG;
    if (s->acquired) return H2_PAL_ERR_INVALID_STATE;
    h2_pal_result_t rc = drop_configuration(s);
    if (rc != H2_PAL_OK) return rc;
    const h2_pal_mem_api_t allocator = s->allocator;
    h2_pal_mem_free(&allocator, s);
    return H2_PAL_OK;
}
const h2_pal_audio_decoder_api_t *h2_ios_platform_audio_decoder_api(void) {
    static const h2_pal_audio_decoder_vtable_t vtable = {
        .open = decoder_open, .configure = decoder_configure,
        .submit_packet = decoder_submit, .acquire_frame = decoder_acquire,
        .frame_get_info = decoder_info, .release_frame = decoder_release,
        .reset = decoder_reset, .close = decoder_close};
    static const h2_pal_audio_decoder_api_t api = {.vtable = &vtable};
    return &api;
}
