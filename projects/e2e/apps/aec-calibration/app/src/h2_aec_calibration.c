#include "h2_aec_calibration.h"

#include <limits.h>
#include <string.h>

/* Q15 sine, shared by stimulus synthesis and the measurement basis. */
static const int16_t sine[256] = {
    0, 804, 1608, 2410, 3212, 4011, 4808, 5602,
    6393, 7179, 7962, 8739, 9512, 10278, 11039, 11793,
    12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530,
    18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594,
    23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790,
    27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
    30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971,
    32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757,
    32767, 32757, 32728, 32678, 32609, 32521, 32412, 32285,
    32137, 31971, 31785, 31580, 31356, 31113, 30852, 30571,
    30273, 29956, 29621, 29268, 28898, 28510, 28105, 27683,
    27245, 26790, 26319, 25832, 25329, 24811, 24279, 23731,
    23170, 22594, 22005, 21403, 20787, 20159, 19519, 18868,
    18204, 17530, 16846, 16151, 15446, 14732, 14010, 13279,
    12539, 11793, 11039, 10278, 9512, 8739, 7962, 7179,
    6393, 5602, 4808, 4011, 3212, 2410, 1608, 804,
    0, -804, -1608, -2410, -3212, -4011, -4808, -5602,
    -6393, -7179, -7962, -8739, -9512, -10278, -11039, -11793,
    -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530,
    -18204, -18868, -19519, -20159, -20787, -21403, -22005, -22594,
    -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790,
    -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956,
    -30273, -30571, -30852, -31113, -31356, -31580, -31785, -31971,
    -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
    -32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285,
    -32137, -31971, -31785, -31580, -31356, -31113, -30852, -30571,
    -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683,
    -27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731,
    -23170, -22594, -22005, -21403, -20787, -20159, -19519, -18868,
    -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
    -12539, -11793, -11039, -10278, -9512, -8739, -7962, -7179,
    -6393, -5602, -4808, -4011, -3212, -2410, -1608, -804,
};

struct h2_aec_calibration {
    h2_aec_calibration_config_t config;
    h2_aec_calibration_result_t result;
    h2_aec_calibration_measurement_t *meter;
    h2_pal_audio_track_t *track;
    bool mic_started;
    bool speaker_started;
    bool observer_registered;
    bool near_retained;
    bool saved;
    bool ran;
    h2_aec_calibration_pair_t original;
    uint32_t capture_skip;
    uint32_t capture_remaining;
    uint32_t playback_skip;
    uint32_t playback_remaining;
    uint64_t sequence;
    h2_pal_result_t playback_rc;
    int16_t far_probe[H2_AEC_CALIBRATION_MAX_SAMPLES];
    int16_t near_probe[H2_AEC_CALIBRATION_MAX_SAMPLES];
    int16_t capture[H2_AEC_CALIBRATION_MAX_SAMPLES];
    int16_t basis_sin[H2_AEC_CALIBRATION_BANDS][H2_AEC_CALIBRATION_MAX_SAMPLES];
    int16_t basis_cos[H2_AEC_CALIBRATION_BANDS][H2_AEC_CALIBRATION_MAX_SAMPLES];
};

h2_aec_calibration_limits_t h2_aec_calibration_default_limits(void) {
    return (h2_aec_calibration_limits_t){
        .warmup_frames = 64u,
        .measurement_frames = 96u,
        .stability_frames = 640u,
        .io_timeout_ms = 1000u,
        .source_timeout_ms = 5000u,
        .max_cadence_milli = 1250u,
        .cadence_margin_ms = 100u,
        .max_echo_residual_milli = 100u,
        .min_near_retention_milli = 250u,
        .min_double_talk_retention_milli = 500u,
        .min_reference_power = 1000u,
        .min_signal_noise_ratio = 10u,
        .peak_limit = 30000u,
        .amplitude = {2048u, 4096u},
    };
}

static bool valid_limits(const h2_aec_calibration_limits_t *l) {
    return l->warmup_frames >= 4u && l->warmup_frames <= 4096u &&
        l->measurement_frames >= 8u && l->measurement_frames <= 4096u &&
        l->stability_frames >= l->measurement_frames && l->stability_frames <= 4096u &&
        l->io_timeout_ms > 0u && l->io_timeout_ms <= 10000u &&
        l->source_timeout_ms > 0u && l->source_timeout_ms <= 30000u &&
        l->max_cadence_milli >= 1000u && l->max_cadence_milli <= 2000u &&
        l->cadence_margin_ms <= 1000u &&
        l->max_echo_residual_milli > 0u && l->max_echo_residual_milli < 1000u &&
        l->min_near_retention_milli > 0u && l->min_near_retention_milli <= 1000u &&
        l->min_double_talk_retention_milli > 0u &&
        l->min_double_talk_retention_milli <= 1000u &&
        l->min_reference_power > 0u && l->min_signal_noise_ratio >= 2u &&
        l->min_signal_noise_ratio <= 1000u && l->peak_limit > 0u &&
        l->peak_limit < 32760u && l->amplitude[0] > 0u &&
        l->amplitude[0] < l->amplitude[1] &&
        (uint32_t)l->amplitude[1] * 3u < l->peak_limit;
}

h2_pal_result_t h2_aec_calibration_create(const h2_aec_calibration_config_t *c,
                                         h2_aec_calibration_t **out) {
    if (out == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    *out = NULL;
    if (c == NULL || c->audio == NULL || c->mem == NULL || c->mem->vtable == NULL ||
        c->mem->vtable->alloc == NULL || c->mem->vtable->free == NULL ||
        c->time == NULL || c->candidates == NULL || c->candidate_count == 0u ||
        c->candidate_count > H2_AEC_CALIBRATION_MAX_CANDIDATES ||
        c->selection < H2_AEC_CALIBRATION_PARETO_ONLY ||
        c->selection > H2_AEC_CALIBRATION_MIC_FIRST || !valid_limits(&c->limits))
        return H2_PAL_ERR_INVALID_ARG;
    for (size_t i = 0u; i < c->candidate_count; ++i) {
        if (c->candidates[i].speaker_percent == 0u || c->candidates[i].speaker_percent > 100u ||
            c->candidates[i].mic_percent > 100u)
            return H2_PAL_ERR_INVALID_ARG;
        for (size_t j = 0u; j < i; ++j)
            if (c->candidates[i].speaker_percent == c->candidates[j].speaker_percent &&
                c->candidates[i].mic_percent == c->candidates[j].mic_percent)
                return H2_PAL_ERR_INVALID_ARG;
    }
    h2_aec_calibration_t *r = h2_pal_mem_alloc(c->mem, sizeof(*r));
    if (r == NULL)
        return H2_PAL_ERR_NO_MEMORY;
    memset(r, 0, sizeof(*r));
    r->config = *c;
    r->config.candidates = NULL;
    r->result.count = c->candidate_count;
    r->result.selection = c->selection;
    r->result.limits = c->limits;
    for (size_t i = 0u; i < c->candidate_count; ++i) {
        r->result.candidates[i].requested = c->candidates[i];
        r->result.candidates[i].result = H2_PAL_ERR_UNAVAILABLE;
    }
    *out = r;
    return H2_PAL_OK;
}

static bool valid_frame(const h2_aec_calibration_t *r, const h2_audio_frame_t *f,
                        uint8_t channels) {
    const uint16_t n = r->result.format.frame_samples_per_channel;
    return f != NULL && f->data != NULL && f->sample_format == H2_AUDIO_SAMPLE_S16LE &&
        f->sample_rate_hz == r->result.format.sample_rate_hz && f->channels == channels &&
        f->samples_per_channel == n &&
        f->bytes == (size_t)n * channels * sizeof(int16_t) && f->capacity >= f->bytes;
}

static int16_t sample_at(const void *data, size_t index) {
    const uint8_t *bytes = data;
    const uint16_t bits = (uint16_t)bytes[index * 2u] |
        ((uint16_t)bytes[index * 2u + 1u] << 8u);
    return bits <= INT16_MAX ? (int16_t)bits : (int16_t)((int32_t)bits - 65536);
}

static uint32_t magnitude(int16_t s) {
    return s < 0 ? (uint32_t)(-(int32_t)s) : (uint32_t)s;
}

static void sample_stats(int16_t value, uint64_t *energy, uint32_t *peak,
                          uint64_t *clipped, uint32_t limit) {
    const uint32_t m = magnitude(value);
    *energy += (uint64_t)((int64_t)value * value);
    if (m > *peak)
        *peak = m;
    if (m >= limit)
        ++*clipped;
}

static uint64_t band_power(const h2_aec_calibration_t *r, const void *pcm,
                            uint8_t stride, uint8_t lane, unsigned band) {
    int64_t re = 0, im = 0;
    const size_t n = r->result.format.frame_samples_per_channel;
    for (size_t i = 0u; i < n; ++i) {
        const int32_t v = sample_at(pcm, i * stride + lane);
        re += (int64_t)v * r->basis_cos[band][i];
        im += (int64_t)v * r->basis_sin[band][i];
    }
    /* Normalize before squaring: the full Q15 dot product need not fit squared. */
    re /= (int64_t)n * 32768;
    im /= (int64_t)n * 32768;
    return 2u * (uint64_t)(re * re + im * im);
}

static void observe_capture(void *user, const h2_audio_aec_frame_t *f) {
    h2_aec_calibration_t *r = user;
    h2_aec_calibration_measurement_t *m = r->meter;
    if (f == NULL || f->raw == NULL || f->raw->channels < 2u || f->raw->channels > 8u ||
        !valid_frame(r, f->raw, f->raw->channels) ||
        !valid_frame(r, f->reference_input, 1u) || !valid_frame(r, f->processed, 1u) ||
        f->microphone_lane >= f->raw->channels || f->reference_lane >= f->raw->channels ||
        f->microphone_lane == f->reference_lane || f->sequence == 0u ||
        (f->microphone_mask & (1u << f->microphone_lane)) == 0u ||
        (f->microphone_mask & (1u << f->reference_lane)) != 0u ||
        (unsigned)f->microphone_mask >= (1u << f->raw->channels) ||
        (r->sequence != 0u && f->sequence != r->sequence + 1u)) {
        m->diagnostic_rc = H2_PAL_ERR_FORMAT;
        return;
    }
    r->sequence = f->sequence;
    if (r->capture_skip != 0u) {
        --r->capture_skip;
        return;
    }
    if (r->capture_remaining == 0u)
        return;
    if (m->frames != 0u && (m->raw_channels != f->raw->channels ||
        m->microphone_mask != f->microphone_mask || m->microphone_lane != f->microphone_lane ||
        m->reference_lane != f->reference_lane)) {
        m->diagnostic_rc = H2_PAL_ERR_FORMAT;
        return;
    }
    --r->capture_remaining;
    ++m->frames;
    m->raw_channels = f->raw->channels;
    m->microphone_mask = f->microphone_mask;
    m->microphone_lane = f->microphone_lane;
    m->reference_lane = f->reference_lane;
    m->samples += f->raw->samples_per_channel;
    const void *raw = f->raw->data;
    const void *reference = f->reference_input->data;
    const void *output = f->processed->data;
    for (size_t i = 0u; i < f->raw->samples_per_channel; ++i) {
        sample_stats(sample_at(raw, i * f->raw->channels + f->microphone_lane),
                     &m->mic_energy, &m->mic_peak, &m->clipped, r->config.limits.peak_limit);
        sample_stats(sample_at(raw, i * f->raw->channels + f->reference_lane),
                     &m->reference_energy, &m->reference_peak, &m->clipped,
                     r->config.limits.peak_limit);
        sample_stats(sample_at(reference, i), &m->aec_reference_energy, &m->aec_reference_peak,
                     &m->clipped, r->config.limits.peak_limit);
        sample_stats(sample_at(output, i), &m->output_energy, &m->output_peak, &m->clipped,
                     r->config.limits.peak_limit);
        for (uint8_t lane = 0u; lane < f->raw->channels; ++lane) {
            if (lane != f->microphone_lane && (f->microphone_mask & (1u << lane)) != 0u) {
                uint64_t unused_energy = 0u;
                sample_stats(sample_at(raw, i * f->raw->channels + lane), &unused_energy,
                             &m->mic_peak, &m->clipped, r->config.limits.peak_limit);
            }
        }
    }
    for (unsigned b = 0u; b < H2_AEC_CALIBRATION_BANDS; ++b) {
        m->near_band_mic[b] += band_power(r, raw, f->raw->channels, f->microphone_lane, b);
        m->near_band_output[b] += band_power(r, output, 1u, 0u, b);
    }
}

static void observe_playback(void *user, const h2_audio_frame_t *f) {
    h2_aec_calibration_t *r = user;
    if (!valid_frame(r, f, 1u)) {
        r->playback_rc = H2_PAL_ERR_FORMAT;
        return;
    }
    if (r->playback_skip != 0u) {
        --r->playback_skip;
        return;
    }
    if (r->playback_remaining == 0u)
        return;
    --r->playback_remaining;
    ++r->meter->playback_frames;
    for (size_t i = 0u; i < f->samples_per_channel; ++i) {
        const uint32_t m = magnitude(sample_at(f->data, i));
        if (m > r->meter->playback_peak)
            r->meter->playback_peak = m;
        if (m >= r->config.limits.peak_limit)
            ++r->meter->playback_clipped;
    }
}

static void observe_error(void *user, int capture, int result) {
    h2_aec_calibration_t *r = user;
    if (capture)
        r->meter->diagnostic_rc = result == H2_PAL_OK ? H2_PAL_ERR_IO : result;
    else
        r->playback_rc = result == H2_PAL_OK ? H2_PAL_ERR_IO : result;
}

static int control_near_source(h2_aec_calibration_t *r, bool enabled) {
    uint64_t start = 0u, end = 0u;
    int rc = h2_pal_time_get_monotonic_ms(r->config.time, &start);
    if (rc != H2_PAL_OK)
        return rc;
    if (r->meter->source_control_calls == UINT32_MAX)
        return H2_PAL_ERR_IO;
    ++r->meter->source_control_calls;
    rc = r->config.near_source(r->config.near_source_user, enabled,
        &r->result.format, r->near_probe, r->result.format.frame_samples_per_channel,
        r->config.limits.source_timeout_ms);
    const int clock_rc = h2_pal_time_get_monotonic_ms(r->config.time, &end);
    if (clock_rc != H2_PAL_OK)
        return rc != H2_PAL_OK ? rc : clock_rc;
    if (end < start || UINT64_MAX - r->meter->source_control_ms < end - start)
        return H2_PAL_ERR_IO;
    r->meter->source_control_ms += end - start;
    if (end - start > r->meter->source_control_max_ms)
        r->meter->source_control_max_ms = end - start;
    if (rc == H2_PAL_OK && end - start > r->config.limits.source_timeout_ms)
        return H2_PAL_ERR_TIMEOUT;
    return rc;
}

static int stop_phase(h2_aec_calibration_t *r) {
    const h2_pal_audio_api_t *a = r->config.audio;
    int rc, first = H2_PAL_OK;
    if (r->near_retained) {
        rc = control_near_source(r, false);
        if (rc != H2_PAL_OK)
            first = rc;
        else
            r->near_retained = false;
    }
    if (r->mic_started) {
        rc = h2_pal_audio_stop_mic(a);
        if (rc != H2_AUDIO_OK) {
            if (first == H2_PAL_OK)
                first = rc;
        } else {
            r->mic_started = false;
        }
    }
    if (r->track != NULL) {
        rc = h2_pal_audio_track_close(r->track);
        if (rc != H2_AUDIO_OK) {
            if (first == H2_PAL_OK)
                first = rc;
        } else {
            r->track = NULL;
        }
    }
    /* The provider may destroy attached tracks during speaker stop. Retain a
     * failed-close handle with its speaker, instead of making retry use freed
     * storage. A failed independent source stop does not keep DUT audio on. */
    if (r->speaker_started && r->track == NULL) {
        rc = h2_pal_audio_stop_speaker(a);
        if (rc != H2_AUDIO_OK) {
            if (first == H2_PAL_OK)
                first = rc;
        } else {
            r->speaker_started = false;
        }
    }
    return first;
}

h2_pal_result_t h2_aec_calibration_cleanup(h2_aec_calibration_t *r) {
    if (r == NULL)
        return H2_PAL_OK;
    int rc = stop_phase(r);
    if (rc == H2_PAL_OK && r->observer_registered) {
        rc = h2_pal_audio_set_aec_observer(r->config.audio, NULL);
        if (rc == H2_PAL_OK)
            r->observer_registered = false;
    }
    if (rc == H2_PAL_OK && r->saved) {
        rc = h2_pal_audio_set_mic_gain_percent(r->config.audio, r->original.mic_percent);
        if (rc == H2_PAL_OK)
            rc = h2_pal_audio_set_speaker_volume_percent(r->config.audio,
                                                         r->original.speaker_percent);
        uint32_t mic = 0u, speaker = 0u;
        if (rc == H2_PAL_OK)
            rc = h2_pal_audio_get_mic_gain_percent(r->config.audio, &mic);
        if (rc == H2_PAL_OK)
            rc = h2_pal_audio_get_speaker_volume_percent(r->config.audio, &speaker);
        if (rc == H2_PAL_OK && (mic != r->original.mic_percent ||
                                speaker != r->original.speaker_percent))
            rc = H2_PAL_ERR_IO;
        if (rc == H2_PAL_OK)
            r->saved = false;
    }
    r->result.cleanup_rc = rc;
    r->result.retained = rc != H2_PAL_OK;
    if (rc != H2_PAL_OK)
        r->result.selected = false;
    return rc;
}

h2_pal_result_t h2_aec_calibration_destroy(h2_aec_calibration_t *r) {
    if (r == NULL)
        return H2_PAL_OK;
    const int rc = h2_aec_calibration_cleanup(r);
    if (rc != H2_PAL_OK)
        return rc;
    const h2_pal_mem_api_t *mem = r->config.mem;
    memset(r, 0, sizeof(*r));
    h2_pal_mem_free(mem, r);
    return H2_PAL_OK;
}

static int prepare_format(h2_aec_calibration_t *r) {
    h2_audio_info_t info = {0};
    int rc = h2_pal_audio_get_info(r->config.audio, &info);
    if (rc != H2_AUDIO_OK)
        return rc;
    const h2_audio_pcm_format_t f = info.mic_format;
    if (!info.available || !info.mic_supported || !info.playback_supported ||
        f.channels != 1u || f.sample_format != H2_AUDIO_SAMPLE_S16LE ||
        f.sample_rate_hz < 8000u || f.sample_rate_hz > 48000u ||
        f.frame_samples_per_channel < 256u ||
        f.frame_samples_per_channel > H2_AEC_CALIBRATION_MAX_SAMPLES ||
        info.playback_format.sample_rate_hz != f.sample_rate_hz ||
        info.playback_format.frame_samples_per_channel != f.frame_samples_per_channel ||
        info.playback_format.channels != 1u ||
        info.playback_format.sample_format != H2_AUDIO_SAMPLE_S16LE)
        return H2_PAL_ERR_UNSUPPORTED;
    r->result.format = f;
    static const uint16_t frequencies[6] = {437u, 1031u, 2156u, 719u, 1438u, 2938u};
    uint16_t bins[6];
    for (unsigned i = 0u; i < 6u; ++i) {
        bins[i] = (uint16_t)(((uint32_t)frequencies[i] * f.frame_samples_per_channel +
                              f.sample_rate_hz / 2u) / f.sample_rate_hz);
        if (bins[i] == 0u || bins[i] >= f.frame_samples_per_channel / 2u)
            return H2_PAL_ERR_UNSUPPORTED;
        for (unsigned j = 0u; j < i; ++j)
            if (bins[i] == bins[j])
                return H2_PAL_ERR_UNSUPPORTED;
    }
    memcpy(r->result.far_bins, bins, sizeof(r->result.far_bins));
    memcpy(r->result.near_bins, bins + 3u, sizeof(r->result.near_bins));
    for (unsigned b = 0u; b < H2_AEC_CALIBRATION_BANDS; ++b)
        for (size_t i = 0u; i < f.frame_samples_per_channel; ++i) {
            const unsigned phase = (unsigned)(i * bins[b + 3u] * 256u /
                                               f.frame_samples_per_channel) % 256u;
            r->basis_sin[b][i] = sine[phase];
            r->basis_cos[b][i] = sine[(phase + 64u) % 256u];
        }
    return H2_PAL_OK;
}

static void prepare_probes(h2_aec_calibration_t *r, unsigned level) {
    const size_t n = r->result.format.frame_samples_per_channel;
    const int32_t amp = r->config.limits.amplitude[level];
    for (size_t i = 0u; i < n; ++i) {
        int32_t far = 0, near = 0;
        for (unsigned b = 0u; b < H2_AEC_CALIBRATION_BANDS; ++b) {
            const unsigned phase = (unsigned)(i * r->result.far_bins[b] * 256u / n) % 256u;
            far += ((int32_t)sine[phase] * amp) / 32768;
            near += ((int32_t)r->basis_sin[b][i] * amp) / 32768;
        }
        r->far_probe[i] = (int16_t)far;
        r->near_probe[i] = (int16_t)near;
    }
}

static int phase(h2_aec_calibration_t *r, h2_aec_calibration_measurement_t *m,
                  h2_aec_calibration_phase_t which) {
    const bool far = which == H2_AEC_CALIBRATION_FAR ||
        which == H2_AEC_CALIBRATION_DOUBLE_TALK || which == H2_AEC_CALIBRATION_STABILITY;
    const bool near = which == H2_AEC_CALIBRATION_NEAR ||
        which == H2_AEC_CALIBRATION_DOUBLE_TALK || which == H2_AEC_CALIBRATION_STABILITY;
    const uint32_t measured = which == H2_AEC_CALIBRATION_STABILITY ?
        r->config.limits.stability_frames : r->config.limits.measurement_frames;
    r->meter = m;
    r->capture_skip = r->playback_skip = r->config.limits.warmup_frames;
    r->capture_remaining = r->playback_remaining = measured;
    r->sequence = 0u;
    r->playback_rc = H2_PAL_OK;
    r->near_retained = true;
    int rc = control_near_source(r, near);
    if (rc == H2_PAL_OK && !near)
        r->near_retained = false;
    uint64_t start = 0u, end = 0u;
    if (rc == H2_PAL_OK) {
        r->mic_started = true;
        rc = h2_pal_audio_start_mic(r->config.audio);
    }
    if (rc == H2_PAL_OK) {
        r->speaker_started = true;
        rc = h2_pal_audio_start_speaker(r->config.audio);
    }
    const h2_audio_track_config_t track = {
        .name = "aec-calibration/probe", .format = r->result.format,
        .volume_factor_milli = 1000u, .buffer_frames = 4u, .allocator = r->config.mem,
    };
    if (rc == H2_PAL_OK)
        rc = h2_pal_audio_create_track(r->config.audio, &track, &r->track);
    if (rc == H2_PAL_OK)
        rc = h2_pal_time_get_monotonic_ms(r->config.time, &start);
    int16_t silence[H2_AEC_CALIBRATION_MAX_SAMPLES] = {0};
    const size_t bytes = (size_t)r->result.format.frame_samples_per_channel * sizeof(int16_t);
    const uint32_t frames = r->config.limits.warmup_frames + measured;
    for (uint32_t i = 0u; rc == H2_PAL_OK && i < frames; ++i) {
        h2_audio_frame_t out = h2_audio_frame_for_buffer(
            far ? r->far_probe : silence, bytes, r->result.format);
        out.bytes = bytes;
        rc = h2_pal_audio_track_write(r->track, &out, r->config.limits.io_timeout_ms);
        h2_audio_frame_t in = h2_audio_frame_for_buffer(r->capture, sizeof(r->capture),
                                                       r->result.format);
        if (rc == H2_PAL_OK)
            rc = h2_pal_audio_mic_read(r->config.audio, &in, r->config.limits.io_timeout_ms);
        if (rc == H2_PAL_OK && !valid_frame(r, &in, 1u))
            rc = H2_PAL_ERR_FORMAT;
        if (rc == H2_PAL_OK)
            rc = h2_pal_time_get_monotonic_ms(r->config.time, &end);
        const uint64_t nominal_ms = (uint64_t)frames *
            r->result.format.frame_samples_per_channel * 1000u / r->result.format.sample_rate_hz;
        const uint64_t maximum = nominal_ms * r->config.limits.max_cadence_milli / 1000u +
            r->config.limits.cadence_margin_ms;
        if (rc == H2_PAL_OK && (end < start || end - start > maximum))
            rc = H2_PAL_ERR_TIMEOUT;
    }
    if (rc == H2_PAL_OK)
        rc = h2_pal_audio_track_drain(r->track, r->config.limits.io_timeout_ms);
    const int cleaned = stop_phase(r);
    if (cleaned != H2_PAL_OK) {
        r->result.cleanup_rc = cleaned;
        r->result.retained = true;
        return cleaned;
    }
    /* Both worker callbacks are now quiescent; no shared state was read live. */
    m->elapsed_ms = end >= start ? end - start : 0u;
    if (m->diagnostic_rc == H2_PAL_OK && r->playback_rc != H2_PAL_OK)
        m->diagnostic_rc = r->playback_rc;
    if (rc != H2_PAL_OK)
        return rc;
    if (m->diagnostic_rc != H2_PAL_OK)
        return m->diagnostic_rc;
    if (r->playback_rc != H2_PAL_OK)
        return r->playback_rc;
    const uint64_t minimum_ms = (uint64_t)measured *
        r->result.format.frame_samples_per_channel * 1000u / r->result.format.sample_rate_hz;
    if (m->frames != measured || m->playback_frames < measured / 2u ||
        m->elapsed_ms < minimum_ms)
        return H2_PAL_ERR_INVALID_STATE;
    return H2_PAL_OK;
}

static uint64_t above(uint64_t value, uint64_t background) {
    return value > background ? value - background : 0u;
}

static bool ratio_at_least(uint64_t numerator, uint64_t denominator, uint32_t milli) {
    return denominator != 0u && numerator * 1000u >= denominator * milli;
}

static bool accepted_level(const h2_aec_calibration_t *r,
                            const h2_aec_calibration_measurement_t *m) {
    const h2_aec_calibration_limits_t *l = &r->config.limits;
    for (unsigned p = 0u; p < H2_AEC_CALIBRATION_PHASE_COUNT; ++p)
        if (m[p].clipped != 0u || m[p].playback_clipped != 0u ||
            m[p].samples == 0u || m[p].diagnostic_rc != H2_PAL_OK)
            return false;
    const h2_aec_calibration_measurement_t *noise = &m[H2_AEC_CALIBRATION_NOISE];
    const h2_aec_calibration_measurement_t *far = &m[H2_AEC_CALIBRATION_FAR];
    const h2_aec_calibration_measurement_t *near = &m[H2_AEC_CALIBRATION_NEAR];
    const uint64_t noise_power = noise->mic_energy / noise->samples;
    const uint64_t far_power = far->mic_energy / far->samples;
    if (far_power <= (noise_power + 1u) * l->min_signal_noise_ratio ||
        far->reference_energy / far->samples < l->min_reference_power ||
        far->aec_reference_energy / far->samples < l->min_reference_power ||
        far->playback_peak == 0u ||
        above(far->output_energy / far->samples, noise->output_energy / noise->samples) *
            1000u > above(far_power, noise_power) * l->max_echo_residual_milli ||
        near->reference_energy / near->samples >
            far->reference_energy / far->samples / l->min_signal_noise_ratio)
        return false;
    for (unsigned b = 0u; b < H2_AEC_CALIBRATION_BANDS; ++b) {
        const uint64_t background = noise->near_band_mic[b] / noise->frames;
        const uint64_t raw = above(near->near_band_mic[b] / near->frames, background);
        const uint64_t output = above(near->near_band_output[b] / near->frames,
                                       noise->near_band_output[b] / noise->frames);
        if (raw <= (background + 1u) * l->min_signal_noise_ratio ||
            !ratio_at_least(output, raw, l->min_near_retention_milli))
            return false;
        for (unsigned p = H2_AEC_CALIBRATION_DOUBLE_TALK;
             p <= H2_AEC_CALIBRATION_STABILITY; ++p) {
            const uint64_t dt_raw = above(m[p].near_band_mic[b] / m[p].frames,
                                           far->near_band_mic[b] / far->frames);
            const uint64_t dt_out = above(m[p].near_band_output[b] / m[p].frames,
                                           far->near_band_output[b] / far->frames);
            if (m[p].playback_peak == 0u ||
                m[p].reference_energy / m[p].samples < l->min_reference_power ||
                dt_raw < raw / 2u || dt_raw > raw * 2u ||
                !ratio_at_least(dt_out, dt_raw, l->min_near_retention_milli) ||
                !ratio_at_least(dt_out, output, l->min_double_talk_retention_milli))
                return false;
        }
    }
    return true;
}

static void select_candidates(h2_aec_calibration_t *r) {
    h2_aec_calibration_result_t *result = &r->result;
    for (size_t i = 0u; i < result->count; ++i) {
        h2_aec_calibration_candidate_t *a = &result->candidates[i];
        if (a->result != H2_PAL_OK)
            continue;
        ++result->passed;
        a->pareto = true;
        for (size_t j = 0u; j < result->count; ++j) {
            const h2_aec_calibration_candidate_t *b = &result->candidates[j];
            if (b->result == H2_PAL_OK &&
                b->actual.speaker_percent >= a->actual.speaker_percent &&
                b->actual.mic_percent >= a->actual.mic_percent &&
                (b->actual.speaker_percent > a->actual.speaker_percent ||
                 b->actual.mic_percent > a->actual.mic_percent))
                a->pareto = false;
        }
        if (!a->pareto)
            continue;
        ++result->pareto_count;
        if (result->selection == H2_AEC_CALIBRATION_PARETO_ONLY)
            continue;
        const h2_aec_calibration_pair_t previous =
            result->candidates[result->selected_index].actual;
        const bool speaker_first = result->selection == H2_AEC_CALIBRATION_SPEAKER_FIRST;
        const uint32_t primary = speaker_first ? a->actual.speaker_percent : a->actual.mic_percent;
        const uint32_t old_primary = speaker_first ? previous.speaker_percent : previous.mic_percent;
        const uint32_t secondary = speaker_first ? a->actual.mic_percent : a->actual.speaker_percent;
        const uint32_t old_secondary = speaker_first ? previous.mic_percent : previous.speaker_percent;
        if (!result->selected || primary > old_primary ||
            (primary == old_primary && secondary > old_secondary)) {
            result->selected = true;
            result->selected_index = i;
        }
    }
}

h2_pal_result_t h2_aec_calibration_run(h2_aec_calibration_t *r,
                                      const h2_aec_calibration_result_t **out) {
    if (out == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    *out = NULL;
    if (r == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    *out = &r->result;
    if (r->ran)
        return H2_PAL_ERR_INVALID_STATE;
    r->ran = true;
    int rc = prepare_format(r);
    if (rc != H2_PAL_OK) {
        for (size_t i = 0u; i < r->result.count; ++i)
            r->result.candidates[i].result = rc;
        r->result.execution_rc = rc;
        return rc;
    }
    if (r->config.near_source == NULL) {
        for (size_t i = 0u; i < r->result.count; ++i)
            r->result.candidates[i].result = H2_PAL_ERR_UNSUPPORTED;
        r->result.execution_rc = H2_PAL_ERR_UNSUPPORTED;
        return H2_PAL_ERR_UNSUPPORTED;
    }
    const h2_audio_aec_observer_t observer = {
        .user = r, .on_capture = observe_capture, .on_playback = observe_playback,
        .on_error = observe_error,
    };
    if (rc == H2_PAL_OK)
        rc = h2_pal_audio_set_aec_observer(r->config.audio, &observer);
    if (rc == H2_PAL_OK)
        r->observer_registered = true;
    if (rc == H2_PAL_OK)
        rc = h2_pal_audio_get_speaker_volume_percent(r->config.audio, &r->original.speaker_percent);
    if (rc == H2_PAL_OK)
        rc = h2_pal_audio_get_mic_gain_percent(r->config.audio, &r->original.mic_percent);
    if (rc == H2_PAL_OK)
        r->saved = true;
    size_t measured = 0u;
    for (size_t i = 0u; rc == H2_PAL_OK && i < r->result.count; ++i) {
        h2_aec_calibration_candidate_t *candidate = &r->result.candidates[i];
        rc = h2_pal_audio_set_speaker_volume_percent(r->config.audio, candidate->requested.speaker_percent);
        if (rc == H2_PAL_OK)
            rc = h2_pal_audio_set_mic_gain_percent(r->config.audio, candidate->requested.mic_percent);
        if (rc == H2_PAL_OK)
            rc = h2_pal_audio_get_speaker_volume_percent(r->config.audio, &candidate->actual.speaker_percent);
        if (rc == H2_PAL_OK)
            rc = h2_pal_audio_get_mic_gain_percent(r->config.audio, &candidate->actual.mic_percent);
        if (rc == H2_PAL_OK && (candidate->actual.speaker_percent == 0u ||
            candidate->actual.speaker_percent > 100u ||
            candidate->actual.mic_percent > 100u))
            rc = H2_PAL_ERR_IO;
        bool accepted = true;
        for (unsigned level = 0u; rc == H2_PAL_OK && level < H2_AEC_CALIBRATION_LEVELS; ++level) {
            prepare_probes(r, level);
            for (unsigned p = 0u; rc == H2_PAL_OK && p < H2_AEC_CALIBRATION_PHASE_COUNT; ++p)
                rc = phase(r, &candidate->measurements[level][p], (h2_aec_calibration_phase_t)p);
            if (rc == H2_PAL_OK && !accepted_level(r, candidate->measurements[level]))
                accepted = false;
        }
        candidate->result = rc == H2_PAL_OK ? (accepted ? H2_PAL_OK : H2_PAL_ERR_IO) : rc;
        if (rc == H2_PAL_OK)
            ++measured;
    }
    r->result.complete = measured == r->result.count;
    if (r->result.complete)
        select_candidates(r);
    const int cleanup = h2_aec_calibration_cleanup(r);
    if (cleanup != H2_PAL_OK) {
        r->result.execution_rc = cleanup;
        return cleanup;
    }
    r->result.execution_rc = rc != H2_PAL_OK ? rc :
        (r->result.passed > 0u ? H2_PAL_OK : H2_PAL_ERR_IO);
    return r->result.execution_rc;
}
