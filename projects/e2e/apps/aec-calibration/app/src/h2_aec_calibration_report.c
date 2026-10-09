#include "h2_aec_calibration.h"

#include <inttypes.h>
#include <stdio.h>

#define PREFIX "AEC_CALIBRATION {\"run\":\"%016" PRIx64 "\","

static int send(h2_aec_calibration_emit_fn emit, void *user, const char *line,
                  int length, size_t capacity) {
    if (length < 0 || (size_t)length >= capacity)
        return H2_PAL_ERR_TRUNCATED;
    return emit(user, line, (size_t)length);
}

h2_pal_result_t h2_aec_calibration_report(const h2_aec_calibration_result_t *r,
                                          uint64_t run, h2_aec_calibration_emit_fn emit,
                                          void *user) {
    if (r == NULL || emit == NULL || r->count > H2_AEC_CALIBRATION_MAX_CANDIDATES)
        return H2_PAL_ERR_INVALID_ARG;
    char line[2048];
    const h2_aec_calibration_limits_t *l = &r->limits;
    int n = snprintf(line, sizeof(line), PREFIX
        "\"kind\":\"begin\",\"schema\":1,\"probe\":\"three-band-two-level-v1\","
        "\"count\":%u,\"selection\":%u,\"sample_rate\":%u,\"frame_samples\":%u,"
        "\"far_bins\":[%u,%u,%u],\"near_bins\":[%u,%u,%u],"
        "\"amplitude\":[%u,%u],\"warmup\":%u,\"measurement\":%u,\"stability\":%u,"
        "\"io_timeout_ms\":%u,\"source_timeout_ms\":%u,\"max_cadence_milli\":%u,\"cadence_margin_ms\":%u,"
        "\"max_residual_milli\":%u,\"min_near_milli\":%u,\"min_double_milli\":%u,"
        "\"min_reference_power\":%u,\"min_snr\":%u,\"peak_limit\":%u}",
        run, (unsigned)r->count, (unsigned)r->selection,
        (unsigned)r->format.sample_rate_hz, (unsigned)r->format.frame_samples_per_channel,
        r->far_bins[0], r->far_bins[1], r->far_bins[2],
        r->near_bins[0], r->near_bins[1], r->near_bins[2], l->amplitude[0], l->amplitude[1],
        (unsigned)l->warmup_frames, (unsigned)l->measurement_frames, (unsigned)l->stability_frames,
        (unsigned)l->io_timeout_ms, (unsigned)l->source_timeout_ms,
        (unsigned)l->max_cadence_milli, (unsigned)l->cadence_margin_ms,
        (unsigned)l->max_echo_residual_milli, (unsigned)l->min_near_retention_milli,
        (unsigned)l->min_double_talk_retention_milli, (unsigned)l->min_reference_power,
        (unsigned)l->min_signal_noise_ratio, (unsigned)l->peak_limit);
    int rc = send(emit, user, line, n, sizeof(line));
    for (size_t i = 0u; rc == H2_PAL_OK && i < r->count; ++i) {
        const h2_aec_calibration_candidate_t *c = &r->candidates[i];
        n = snprintf(line, sizeof(line), PREFIX
            "\"kind\":\"candidate\",\"index\":%u,\"requested\":[%u,%u],"
            "\"actual\":[%u,%u],\"rc\":%d,\"pareto\":%s}", run, (unsigned)i,
            (unsigned)c->requested.speaker_percent, (unsigned)c->requested.mic_percent,
            (unsigned)c->actual.speaker_percent, (unsigned)c->actual.mic_percent,
            c->result, c->pareto ? "true" : "false");
        rc = send(emit, user, line, n, sizeof(line));
        for (unsigned level = 0u; rc == H2_PAL_OK && level < H2_AEC_CALIBRATION_LEVELS; ++level)
            for (unsigned p = 0u; rc == H2_PAL_OK && p < H2_AEC_CALIBRATION_PHASE_COUNT; ++p) {
                const h2_aec_calibration_measurement_t *m = &c->measurements[level][p];
                n = snprintf(line, sizeof(line), PREFIX
                    "\"kind\":\"phase\",\"index\":%u,\"level\":%u,\"phase\":%u,"
                    "\"frames\":%u,\"playback_frames\":%u,\"samples\":%" PRIu64 ","
                    "\"raw_channels\":%u,\"mic_mask\":%u,\"mic_lane\":%u,\"reference_lane\":%u,"
                    "\"elapsed_ms\":%" PRIu64 ",\"source_control_ms\":%" PRIu64 ","
                    "\"source_control_max_ms\":%" PRIu64 ",\"source_control_calls\":%u,\"mic_energy\":%" PRIu64 ","
                    "\"reference_energy\":%" PRIu64 ",\"aec_reference_energy\":%" PRIu64 ","
                    "\"output_energy\":%" PRIu64 ",\"near_mic\":[%" PRIu64 ",%" PRIu64 ",%" PRIu64 "],"
                    "\"near_output\":[%" PRIu64 ",%" PRIu64 ",%" PRIu64 "],"
                    "\"peak\":[%u,%u,%u,%u],\"clipped\":%" PRIu64 ","
                    "\"playback_peak\":%u,\"playback_clipped\":%" PRIu64 ",\"diagnostic_rc\":%d}",
                    run, (unsigned)i, level, p, (unsigned)m->frames, (unsigned)m->playback_frames,
                    m->samples, (unsigned)m->raw_channels, (unsigned)m->microphone_mask,
                    (unsigned)m->microphone_lane, (unsigned)m->reference_lane,
                    m->elapsed_ms, m->source_control_ms,
                    m->source_control_max_ms, (unsigned)m->source_control_calls,
                    m->mic_energy, m->reference_energy,
                    m->aec_reference_energy, m->output_energy,
                    m->near_band_mic[0], m->near_band_mic[1], m->near_band_mic[2],
                    m->near_band_output[0], m->near_band_output[1], m->near_band_output[2],
                    (unsigned)m->mic_peak, (unsigned)m->reference_peak,
                    (unsigned)m->aec_reference_peak, (unsigned)m->output_peak, m->clipped,
                    (unsigned)m->playback_peak, m->playback_clipped, m->diagnostic_rc);
                rc = send(emit, user, line, n, sizeof(line));
            }
    }
    if (rc != H2_PAL_OK)
        return rc;
    n = snprintf(line, sizeof(line), PREFIX
        "\"kind\":\"summary\",\"count\":%u,\"passed\":%u,\"pareto_count\":%u,"
        "\"selected\":%s,\"selected_index\":%u,\"complete\":%s,\"retained\":%s,\"rc\":%d,\"cleanup_rc\":%d}",
        run, (unsigned)r->count, (unsigned)r->passed, (unsigned)r->pareto_count,
        r->selected ? "true" : "false", (unsigned)r->selected_index,
        r->complete ? "true" : "false", r->retained ? "true" : "false", r->execution_rc, r->cleanup_rc);
    return send(emit, user, line, n, sizeof(line));
}
