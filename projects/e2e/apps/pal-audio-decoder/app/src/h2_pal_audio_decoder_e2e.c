#include "h2_pal_audio_decoder_e2e.h"
#include "h2_aac_vectors.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef union allocation_header {
    size_t size;
#if defined(_MSC_VER) && !defined(__clang__)
    long double alignment;
    long long integer_alignment;
    void *pointer_alignment;
#else
    max_align_t alignment;
#endif
} allocation_header_t;

typedef struct state {
    h2_pal_adec_config_t config;
    h2_pal_adec_result_t *result;
    h2_pal_mem_api_t memory;
    size_t live, bytes, calls, fail_at;
    unsigned line;
    h2_pal_audio_decoder_session_t *session;
    h2_pal_audio_decoder_frame_t *frame;
    h2_pal_mutex_t *memory_mutex;
} state_t;

#define CHECK(s, expression) do { if (!(expression)) { (s)->line = __LINE__; return H2_PAL_ERR_INVALID_STATE; } } while (0)
#define OK(s, expression) do { h2_pal_result_t status_ = (expression); if (status_ != H2_PAL_OK) { (s)->line = __LINE__; return status_; } } while (0)
#define API(s) ((s)->config.decoder)

static void *allocate(void *user, size_t size) {
    state_t *s = user;
    if (h2_pal_mutex_lock(s->config.sync, s->memory_mutex) != H2_PAL_OK) return NULL;
    if (s->calls++ == s->fail_at || size > SIZE_MAX - sizeof(allocation_header_t)) {
        (void)h2_pal_mutex_unlock(s->config.sync, s->memory_mutex);
        return NULL;
    }
    allocation_header_t *header = h2_pal_mem_alloc(s->config.mem, sizeof(*header) + size);
    if (!header) {
        (void)h2_pal_mutex_unlock(s->config.sync, s->memory_mutex);
        return NULL;
    }
    header->size = size;
    ++s->live;
    s->bytes += size;
    (void)h2_pal_mutex_unlock(s->config.sync, s->memory_mutex);
    return header + 1;
}
static void release(void *user, void *pointer) {
    state_t *s = user;
    if (!pointer) return;
    if (h2_pal_mutex_lock(s->config.sync, s->memory_mutex) != H2_PAL_OK) return;
    allocation_header_t *header = (allocation_header_t *)pointer - 1;
    if (!s->live || s->bytes < header->size) {
        s->live = SIZE_MAX;
        (void)h2_pal_mutex_unlock(s->config.sync, s->memory_mutex);
        return;
    }
    --s->live;
    s->bytes -= header->size;
    h2_pal_mem_free(s->config.mem, header);
    (void)h2_pal_mutex_unlock(s->config.sync, s->memory_mutex);
}
static void *reallocate(void *user, void *pointer, size_t size) {
    if (!pointer) return allocate(user, size);
    if (!size) { release(user, pointer); return NULL; }
    allocation_header_t *old = (allocation_header_t *)pointer - 1;
    void *replacement = allocate(user, size);
    if (!replacement) return NULL;
    memcpy(replacement, pointer, old->size < size ? old->size : size);
    release(user, pointer);
    return replacement;
}
static const h2_pal_mem_vtable_t memory_vtable = {
    .alloc = allocate, .realloc = reallocate, .free = release};

static int64_t packet_pts(const h2_aac_e2e_vector_t *v, size_t index) {
    return 1000000 + (int64_t)((uint64_t)index * 1024u * 1000000u / v->sample_rate_hz);
}
static h2_audio_decoder_stream_config_t stream_config(const h2_aac_e2e_vector_t *v) {
    return (h2_audio_decoder_stream_config_t){
        .codec = H2_AUDIO_CODEC_AAC_LC, .bitstream_format = H2_AUDIO_BITSTREAM_AAC_RAW,
        .sample_rate_hz = v->sample_rate_hz, .channels = v->channels,
        .codec_config = v->audio_specific_config, .codec_config_size = 2u};
}
static h2_pal_result_t open_session(state_t *s) {
    const h2_audio_decoder_config_t config = {
        .pcm_allocator = &s->memory, .preferred_format = H2_AUDIO_SAMPLE_S16LE};
    return h2_pal_audio_decoder_open(API(s), &config, &s->session);
}
static h2_pal_result_t configure(state_t *s, const h2_aac_e2e_vector_t *v) {
    const h2_audio_decoder_stream_config_t config = stream_config(v);
    return h2_pal_audio_decoder_configure(API(s), s->session, &config);
}
static h2_pal_result_t cleanup(state_t *s) {
    if (s->frame) {
        h2_pal_result_t rc = h2_pal_audio_decoder_release_frame(API(s), s->session, s->frame);
        if (rc != H2_PAL_OK) return rc;
        s->frame = NULL;
    }
    if (s->session) {
        h2_pal_result_t rc = h2_pal_audio_decoder_close(API(s), s->session);
        if (rc != H2_PAL_OK) return rc;
        s->session = NULL;
    }
    return s->live == 0u && s->bytes == 0u ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
}
static h2_pal_result_t clock_ms(state_t *s, uint64_t *now) {
    return h2_pal_time_get_monotonic_ms(s->config.time, now);
}
static void pause_poll(state_t *s) {
    if (s->config.pump) s->config.pump(s->config.pump_user);
    (void)h2_pal_time_sleep_ms(s->config.time, 1u);
}

static h2_pal_result_t submit(state_t *s, const h2_aac_e2e_vector_t *v, size_t index,
                               bool poison) {
    const h2_aac_e2e_packet_t *raw = &v->packets[index];
    uint8_t *copy = NULL;
    if (poison) {
        copy = h2_pal_mem_alloc(s->config.mem, raw->size);
        if (!copy) return H2_PAL_ERR_NO_MEMORY;
        memcpy(copy, raw->data, raw->size);
    }
    const h2_audio_decoder_packet_t packet = {
        .data = copy ? copy : raw->data, .size = raw->size,
        .pts_us = packet_pts(v, index), .dts_us = packet_pts(v, index),
        .duration_us = (int64_t)(1024u * 1000000u / v->sample_rate_hz)};
    h2_pal_result_t rc = h2_pal_audio_decoder_submit_packet(API(s), s->session, &packet);
    if (copy) {
        memset(copy, 0xa5, raw->size);
        h2_pal_mem_free(s->config.mem, copy);
    }
    return rc;
}

/* Score PCM against the original fixture frequencies, independent of codec rounding. */
static double tone_power(const int16_t *samples, uint32_t count, uint8_t channels,
                          unsigned channel, uint32_t rate, uint32_t frequency) {
    const double coefficient = 2.0 * cos(6.283185307179586 * frequency / rate);
    double previous = 0.0, older = 0.0;
    for (uint32_t i = 0u; i < count; ++i) {
        double current = samples[(size_t)i * channels + channel] + coefficient * previous - older;
        older = previous;
        previous = current;
    }
    return previous * previous + older * older - coefficient * previous * older;
}

static h2_pal_result_t inspect_frame(state_t *s, const h2_aac_e2e_vector_t *v,
                                      bool signal, bool timing, size_t *seen,
                                      size_t *signal_frames, int64_t *last_pts) {
    h2_audio_decoder_frame_info_t info = {0};
    OK(s, h2_pal_audio_decoder_frame_get_info(API(s), s->session, s->frame, &info));
    CHECK(s, info.data && info.bytes && info.sample_format == H2_AUDIO_SAMPLE_S16LE);
    CHECK(s, info.sample_rate_hz == v->sample_rate_hz && info.channels == v->channels);
    CHECK(s, info.samples_per_channel > 0u && info.samples_per_channel <= 4096u);
    CHECK(s, info.bytes == (size_t)info.samples_per_channel * v->channels * sizeof(int16_t));
    CHECK(s, info.duration_us > 0 && info.duration_us <= 100000);
    if (timing) {
        CHECK(s, info.pts_us >= 1000000 && info.pts_us >= *last_pts);
        CHECK(s, info.pts_us <= packet_pts(v, v->packet_count));
        *last_pts = info.pts_us;
    }
    if (signal && *seen >= 2u && info.samples_per_channel >= 512u) {
        const int16_t *samples = info.data;
        double energy = 0.0;
        for (size_t i = 0u; i < info.bytes / sizeof(int16_t); ++i)
            energy += (double)samples[i] * samples[i];
        CHECK(s, energy > (double)info.samples_per_channel * v->channels * 10000.0);
        for (unsigned ch = 0u; ch < v->channels; ++ch) {
            double wanted = tone_power(samples, info.samples_per_channel, v->channels, ch,
                                      v->sample_rate_hz, v->tone_hz[ch]);
            double off = tone_power(samples, info.samples_per_channel, v->channels, ch,
                                   v->sample_rate_hz, 5701u);
            CHECK(s, wanted > off * 20.0 && wanted > 1e8);
            if (v->channels == 2u) {
                double other = tone_power(samples, info.samples_per_channel, v->channels, ch,
                                         v->sample_rate_hz, v->tone_hz[1u - ch]);
                CHECK(s, wanted > other * 8.0);
            }
        }
        ++*signal_frames;
    }
    ++*seen;
    ++s->result->frames;
    s->result->pcm_bytes += info.bytes;
    OK(s, h2_pal_audio_decoder_release_frame(API(s), s->session, s->frame));
    s->frame = NULL;
    return H2_PAL_OK;
}

static h2_pal_result_t decode_stream(state_t *s, const h2_aac_e2e_vector_t *v,
                                      bool signal, bool timing, bool poison) {
    uint64_t start = 0u, now = 0u;
    OK(s, clock_ms(s, &start));
    size_t submitted = 0u, seen = 0u, signal_frames = 0u;
    int64_t last_pts = 0;
    bool eos = false;
    for (unsigned iteration = 0u; iteration < 10000u; ++iteration) {
        if (submitted < v->packet_count) {
            h2_pal_result_t rc = submit(s, v, submitted, poison);
            if (rc == H2_PAL_OK) ++submitted;
            else if (rc != H2_PAL_ERR_WOULD_BLOCK) return rc;
        } else if (!eos) {
            const h2_audio_decoder_packet_t packet = {.flags = H2_AUDIO_DECODER_PACKET_END_OF_STREAM};
            h2_pal_result_t rc = h2_pal_audio_decoder_submit_packet(API(s), s->session, &packet);
            if (rc == H2_PAL_OK) eos = true;
            else if (rc != H2_PAL_ERR_WOULD_BLOCK) return rc;
        }
        h2_pal_result_t acquired = h2_pal_audio_decoder_acquire_frame(API(s), s->session, 0u, &s->frame);
        if (acquired == H2_PAL_OK) {
            OK(s, inspect_frame(s, v, signal, timing, &seen, &signal_frames, &last_pts));
        } else if (acquired == H2_PAL_EXIT) {
            CHECK(s, eos && submitted == v->packet_count && seen >= v->packet_count - 2u);
            CHECK(s, !signal || signal_frames >= 3u);
            return H2_PAL_OK;
        } else {
            CHECK(s, acquired == H2_PAL_ERR_WOULD_BLOCK || acquired == H2_PAL_ERR_TIMEOUT);
            pause_poll(s);
        }
        OK(s, clock_ms(s, &now));
        CHECK(s, now - start < 5000u);
    }
    return H2_PAL_ERR_TIMEOUT;
}

static h2_pal_result_t decode_case(state_t *s, const h2_aac_e2e_vector_t *v,
                                   bool signal, bool timing, bool poison) {
    OK(s, open_session(s));
    OK(s, configure(s, v));
    return decode_stream(s, v, signal, timing, poison);
}
static h2_pal_result_t test_arguments(state_t *s) {
    h2_pal_audio_decoder_session_t *session = (h2_pal_audio_decoder_session_t *)(uintptr_t)1;
    CHECK(s, h2_pal_audio_decoder_open(API(s), NULL, &session) == H2_PAL_ERR_INVALID_ARG && !session);
    h2_pal_audio_decoder_frame_t *frame = (h2_pal_audio_decoder_frame_t *)(uintptr_t)1;
    CHECK(s, h2_pal_audio_decoder_acquire_frame(API(s), NULL, 0u, &frame) == H2_PAL_ERR_INVALID_ARG && !frame);
    CHECK(s, h2_pal_audio_decoder_release_frame(API(s), NULL, NULL) == H2_PAL_ERR_INVALID_ARG);
    return H2_PAL_OK;
}
static h2_pal_result_t test_open_close(state_t *s) { return open_session(s); }
static h2_pal_result_t test_preferred_format(state_t *s) {
    const h2_audio_decoder_config_t config = {.pcm_allocator = &s->memory, .preferred_format = (h2_audio_sample_format_t)99};
    h2_pal_result_t rc = h2_pal_audio_decoder_open(API(s), &config, &s->session);
    CHECK(s, (rc == H2_PAL_ERR_INVALID_ARG || rc == H2_PAL_ERR_UNSUPPORTED) && !s->session);
    return H2_PAL_OK;
}
static h2_pal_result_t test_unconfigured(state_t *s) {
    OK(s, open_session(s));
    const h2_audio_decoder_packet_t packet = {.data = h2_aac_e2e_mono.packets[0].data, .size = h2_aac_e2e_mono.packets[0].size};
    CHECK(s, h2_pal_audio_decoder_submit_packet(API(s), s->session, &packet) == H2_PAL_ERR_INVALID_STATE);
    CHECK(s, h2_pal_audio_decoder_acquire_frame(API(s), s->session, 0u, &s->frame) == H2_PAL_ERR_INVALID_STATE && !s->frame);
    return H2_PAL_OK;
}
static h2_pal_result_t test_config_arguments(state_t *s) {
    OK(s, open_session(s));
    h2_audio_decoder_stream_config_t config = stream_config(&h2_aac_e2e_mono);
    config.channels = 0u;
    CHECK(s, h2_pal_audio_decoder_configure(API(s), s->session, &config) == H2_PAL_ERR_INVALID_ARG);
    config = stream_config(&h2_aac_e2e_mono); config.codec_config_size = 0u;
    CHECK(s, h2_pal_audio_decoder_configure(API(s), s->session, &config) == H2_PAL_ERR_INVALID_ARG);
    return H2_PAL_OK;
}
static h2_pal_result_t test_unsupported_codec(state_t *s) {
    OK(s, open_session(s));
    h2_audio_decoder_stream_config_t config = stream_config(&h2_aac_e2e_mono);
    config.codec = (h2_audio_codec_t)99;
    CHECK(s, h2_pal_audio_decoder_configure(API(s), s->session, &config) == H2_PAL_ERR_UNSUPPORTED);
    return H2_PAL_OK;
}
static h2_pal_result_t test_invalid_asc(state_t *s) {
    OK(s, open_session(s));
    const uint8_t invalid[] = {255};
    h2_audio_decoder_stream_config_t config = stream_config(&h2_aac_e2e_mono);
    config.codec_config = invalid; config.codec_config_size = sizeof(invalid);
    h2_pal_result_t rc = h2_pal_audio_decoder_configure(API(s), s->session, &config);
    CHECK(s, rc == H2_PAL_ERR_FORMAT || rc == H2_PAL_ERR_INVALID_ARG || rc == H2_PAL_ERR_UNSUPPORTED);
    return H2_PAL_OK;
}
static h2_pal_result_t test_configure_repeat(state_t *s) {
    OK(s, open_session(s)); OK(s, configure(s, &h2_aac_e2e_mono));
    h2_pal_result_t rc = configure(s, &h2_aac_e2e_mono);
    CHECK(s, rc == H2_PAL_ERR_INVALID_STATE || rc == H2_PAL_ERR_INVALID_ARG);
    return H2_PAL_OK;
}
static h2_pal_result_t test_empty_poll(state_t *s) {
    OK(s, open_session(s)); OK(s, configure(s, &h2_aac_e2e_mono));
    CHECK(s, h2_pal_audio_decoder_acquire_frame(API(s), s->session, 0u, &s->frame) == H2_PAL_ERR_WOULD_BLOCK && !s->frame);
    return H2_PAL_OK;
}
static h2_pal_result_t test_finite_acquire(state_t *s) {
    OK(s, open_session(s)); OK(s, configure(s, &h2_aac_e2e_mono));
    uint64_t before = 0, after = 0; OK(s, clock_ms(s, &before));
    h2_pal_result_t rc = h2_pal_audio_decoder_acquire_frame(API(s), s->session, 20u, &s->frame);
    OK(s, clock_ms(s, &after));
    CHECK(s, (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK) && !s->frame && after - before < 500u);
    return H2_PAL_OK;
}
static h2_pal_result_t test_packet_arguments(state_t *s) {
    OK(s, open_session(s)); OK(s, configure(s, &h2_aac_e2e_mono));
    h2_audio_decoder_packet_t packet = {.data = NULL, .size = 1u};
    CHECK(s, h2_pal_audio_decoder_submit_packet(API(s), s->session, &packet) == H2_PAL_ERR_INVALID_ARG);
    packet.flags = H2_AUDIO_DECODER_PACKET_END_OF_STREAM;
    CHECK(s, h2_pal_audio_decoder_submit_packet(API(s), s->session, &packet) == H2_PAL_ERR_INVALID_ARG);
    packet.size = 0u; packet.flags = 2u;
    CHECK(s, h2_pal_audio_decoder_submit_packet(API(s), s->session, &packet) == H2_PAL_ERR_INVALID_ARG);
    return H2_PAL_OK;
}
static h2_pal_result_t test_mono_pcm(state_t *s) { return decode_case(s, &h2_aac_e2e_mono, false, false, false); }
static h2_pal_result_t test_stereo_pcm(state_t *s) { return decode_case(s, &h2_aac_e2e_stereo, false, false, false); }
static h2_pal_result_t test_mono_signal(state_t *s) { return decode_case(s, &h2_aac_e2e_mono, true, false, false); }
static h2_pal_result_t test_stereo_signal(state_t *s) { return decode_case(s, &h2_aac_e2e_stereo, true, false, false); }
static h2_pal_result_t test_frame_timing(state_t *s) { return decode_case(s, &h2_aac_e2e_stereo, false, true, false); }
static h2_pal_result_t test_borrowed_packet(state_t *s) { return decode_case(s, &h2_aac_e2e_stereo, true, false, true); }
static h2_pal_result_t test_borrowed_asc(state_t *s) {
    OK(s, open_session(s));
    uint8_t asc[2]; memcpy(asc, h2_aac_e2e_stereo.audio_specific_config, sizeof(asc));
    h2_audio_decoder_stream_config_t config = stream_config(&h2_aac_e2e_stereo);
    config.codec_config = asc;
    OK(s, h2_pal_audio_decoder_configure(API(s), s->session, &config));
    memset(asc, 0xa5, sizeof(asc));
    return decode_stream(s, &h2_aac_e2e_stereo, true, false, false);
}

static h2_pal_result_t acquire_one(state_t *s) {
    OK(s, open_session(s)); OK(s, configure(s, &h2_aac_e2e_stereo));
    uint64_t start = 0u, now = 0u; OK(s, clock_ms(s, &start));
    size_t next = 0u;
    for (;;) {
        if (next < h2_aac_e2e_stereo.packet_count) {
            h2_pal_result_t rc = submit(s, &h2_aac_e2e_stereo, next, false);
            if (rc == H2_PAL_OK) ++next; else if (rc != H2_PAL_ERR_WOULD_BLOCK) return rc;
        }
        h2_pal_result_t rc = h2_pal_audio_decoder_acquire_frame(API(s), s->session, 0u, &s->frame);
        if (rc == H2_PAL_OK) return H2_PAL_OK;
        if (rc != H2_PAL_ERR_WOULD_BLOCK && rc != H2_PAL_ERR_TIMEOUT) return rc;
        pause_poll(s); OK(s, clock_ms(s, &now)); CHECK(s, now - start < 5000u);
    }
}
static h2_pal_result_t test_held_acquire(state_t *s) {
    OK(s, acquire_one(s)); h2_pal_audio_decoder_frame_t *other = NULL;
    CHECK(s, h2_pal_audio_decoder_acquire_frame(API(s), s->session, 0u, &other) == H2_PAL_ERR_WOULD_BLOCK && !other);
    return H2_PAL_OK;
}
static h2_pal_result_t test_held_reset(state_t *s) {
    OK(s, acquire_one(s));
    CHECK(s, h2_pal_audio_decoder_reset(API(s), s->session) == H2_PAL_ERR_INVALID_STATE);
    return H2_PAL_OK;
}
static h2_pal_result_t test_held_close(state_t *s) {
    OK(s, acquire_one(s));
    CHECK(s, h2_pal_audio_decoder_close(API(s), s->session) == H2_PAL_ERR_INVALID_STATE);
    return H2_PAL_OK;
}
static h2_pal_result_t test_foreign_frame(state_t *s) {
    OK(s, acquire_one(s));
    h2_pal_audio_decoder_session_t *other = NULL;
    const h2_audio_decoder_config_t config = {.pcm_allocator = &s->memory, .preferred_format = H2_AUDIO_SAMPLE_S16LE};
    OK(s, h2_pal_audio_decoder_open(API(s), &config, &other));
    h2_audio_decoder_frame_info_t info = {0};
    h2_pal_result_t rc = h2_pal_audio_decoder_frame_get_info(API(s), other, s->frame, &info);
    h2_pal_result_t closed = h2_pal_audio_decoder_close(API(s), other);
    CHECK(s, closed == H2_PAL_OK && (rc == H2_PAL_ERR_INVALID_ARG || rc == H2_PAL_ERR_INVALID_STATE) && !info.data);
    return H2_PAL_OK;
}
static h2_pal_result_t test_reset_reconfigure(state_t *s) {
    OK(s, decode_case(s, &h2_aac_e2e_mono, true, false, false));
    OK(s, h2_pal_audio_decoder_reset(API(s), s->session));
    OK(s, configure(s, &h2_aac_e2e_mono));
    return decode_stream(s, &h2_aac_e2e_mono, true, false, false);
}
static h2_pal_result_t test_format_change(state_t *s) {
    OK(s, decode_case(s, &h2_aac_e2e_mono, true, false, false));
    OK(s, h2_pal_audio_decoder_reset(API(s), s->session));
    OK(s, configure(s, &h2_aac_e2e_stereo));
    return decode_stream(s, &h2_aac_e2e_stereo, true, false, false);
}
static h2_pal_result_t test_eos(state_t *s) { return decode_case(s, &h2_aac_e2e_mono, false, true, false); }
static h2_pal_result_t test_post_eos(state_t *s) {
    OK(s, decode_case(s, &h2_aac_e2e_mono, false, false, false));
    CHECK(s, submit(s, &h2_aac_e2e_mono, 0u, false) == H2_PAL_ERR_INVALID_STATE);
    return H2_PAL_OK;
}
static h2_pal_result_t test_allocator_use(state_t *s) {
    OK(s, acquire_one(s));
    OK(s, h2_pal_mutex_lock(s->config.sync, s->memory_mutex));
    const bool used = s->calls && s->live && s->bytes >= 1024u * h2_aac_e2e_stereo.channels * sizeof(int16_t);
    OK(s, h2_pal_mutex_unlock(s->config.sync, s->memory_mutex));
    CHECK(s, used);
    return H2_PAL_OK;
}
static h2_pal_result_t test_allocator_failure(state_t *s) {
    int failed = 0;
    for (size_t index = 0u; index < 12u; ++index) {
        s->calls = 0u; s->fail_at = index;
        h2_pal_result_t rc = acquire_one(s);
        if (rc == H2_PAL_ERR_NO_MEMORY) failed = 1;
        else CHECK(s, rc == H2_PAL_OK);
        s->fail_at = SIZE_MAX;
        OK(s, cleanup(s));
    }
    CHECK(s, failed);
    return H2_PAL_OK;
}
static h2_pal_result_t test_churn(state_t *s) {
    for (unsigned i = 0u; i < 8u; ++i) {
        OK(s, acquire_one(s)); OK(s, cleanup(s));
    }
    return H2_PAL_OK;
}

typedef h2_pal_result_t (*case_fn_t)(state_t *);
static const struct { const char *id; case_fn_t run; } cases[] = {
#define H2_PAL_ADEC_CASE(id, function) {id, function},
#include "h2_pal_audio_decoder_cases.inc"
#undef H2_PAL_ADEC_CASE
};

static bool complete(const h2_pal_audio_decoder_api_t *api) {
    return api && api->vtable && api->vtable->open && api->vtable->configure &&
        api->vtable->submit_packet && api->vtable->acquire_frame &&
        api->vtable->frame_get_info && api->vtable->release_frame &&
        api->vtable->reset && api->vtable->close;
}

h2_pal_result_t h2_pal_audio_decoder_e2e_run(
    const h2_pal_adec_config_t *config, h2_pal_adec_result_t *result) {
    if (!result) return H2_PAL_ERR_INVALID_ARG;
    memset(result, 0, sizeof(*result));
    bool available = config && complete(config->decoder) && config->mem && config->time && config->sync;
    for (size_t i = 0u; i < H2_PAL_ADEC_CASE_COUNT; ++i) {
        h2_pal_adec_case_result_t *item = &result->cases[i];
        item->id = cases[i].id;
        if (!available) {
            item->status = H2_PAL_ADEC_BLOCKED; item->detail = H2_PAL_ERR_UNSUPPORTED;
        } else {
            state_t *state = h2_pal_mem_alloc(config->mem, sizeof(*state));
            if (!state) {
                item->detail = H2_PAL_ERR_NO_MEMORY;
                item->status = H2_PAL_ADEC_FAIL;
                ++result->failed;
                if (config->report) config->report(config->report_user, item);
                continue;
            }
            *state = (state_t){.config = *config, .result = result, .fail_at = SIZE_MAX};
            state->memory = (h2_pal_mem_api_t){.user = state, .vtable = &memory_vtable};
            const h2_pal_mutex_config_t mutex = {.name = "adec/e2e/memory", .allocator = config->mem};
            item->detail = h2_pal_mutex_create(config->sync, &mutex, &state->memory_mutex);
            if (item->detail == H2_PAL_OK) item->detail = cases[i].run(state);
            h2_pal_result_t released = cleanup(state);
            if (released != H2_PAL_OK) item->detail = released;
            if (released == H2_PAL_OK && state->memory_mutex)
                released = h2_pal_mutex_destroy(config->sync, state->memory_mutex);
            if (released != H2_PAL_OK) { item->detail = released; ++result->retained; }
            item->line = item->detail == H2_PAL_OK ? 0u : state->line;
            if (released == H2_PAL_OK) h2_pal_mem_free(config->mem, state);
            item->status = item->detail == H2_PAL_OK ? H2_PAL_ADEC_PASS :
                item->detail == H2_PAL_ERR_UNSUPPORTED ? H2_PAL_ADEC_BLOCKED : H2_PAL_ADEC_FAIL;
        }
        if (item->status == H2_PAL_ADEC_PASS) ++result->passed;
        else if (item->status == H2_PAL_ADEC_BLOCKED) ++result->blocked;
        else ++result->failed;
        if (config && config->report) config->report(config->report_user, item);
    }
    result->qualified = result->passed == H2_PAL_ADEC_CASE_COUNT && !result->failed && !result->blocked && !result->retained;
    return result->qualified ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
}
