#ifndef H2_AEC_CALIBRATION_H
#define H2_AEC_CALIBRATION_H

#include "h2/pal/hal/h2_pal_audio.h"
#include "h2/pal/os/h2_pal_time.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_AEC_CALIBRATION_MAX_CANDIDATES 16u
#define H2_AEC_CALIBRATION_LEVELS 2u
#define H2_AEC_CALIBRATION_BANDS 3u
#define H2_AEC_CALIBRATION_MAX_SAMPLES 1024u

typedef enum h2_aec_calibration_phase {
    H2_AEC_CALIBRATION_NOISE,
    H2_AEC_CALIBRATION_FAR,
    H2_AEC_CALIBRATION_NEAR,
    H2_AEC_CALIBRATION_DOUBLE_TALK,
    H2_AEC_CALIBRATION_STABILITY,
    H2_AEC_CALIBRATION_PHASE_COUNT,
} h2_aec_calibration_phase_t;

typedef enum h2_aec_calibration_selection {
    H2_AEC_CALIBRATION_PARETO_ONLY = 0,
    H2_AEC_CALIBRATION_SPEAKER_FIRST,
    H2_AEC_CALIBRATION_MIC_FIRST,
} h2_aec_calibration_selection_t;

typedef struct h2_aec_calibration_pair {
    uint32_t speaker_percent;
    uint32_t mic_percent;
} h2_aec_calibration_pair_t;

/** All energy fields are sums over complete measured frames, before background
 * subtraction. mic_energy and near_band_mic use the reported primary mic lane;
 * mic_peak/clipped also check every active microphone in microphone_mask.
 * near_band_* are per-band mean-square estimates summed over frames. The
 * report contains metadata only, never microphone PCM. */
typedef struct h2_aec_calibration_measurement {
    uint32_t frames;
    uint32_t playback_frames;
    uint32_t playback_active_frames;
    uint8_t raw_channels;
    uint8_t microphone_mask;
    uint8_t microphone_lane;
    uint8_t reference_lane;
    uint64_t samples;
    uint64_t elapsed_ms;
    uint64_t source_control_ms;
    uint64_t source_control_max_ms;
    uint32_t source_control_calls;
    uint64_t mic_energy;
    uint64_t reference_energy;
    uint64_t aec_reference_energy;
    uint64_t output_energy;
    uint64_t near_band_mic[H2_AEC_CALIBRATION_BANDS];
    uint64_t near_band_output[H2_AEC_CALIBRATION_BANDS];
    uint32_t mic_peak;
    uint32_t reference_peak;
    uint32_t aec_reference_peak;
    uint32_t output_peak;
    uint32_t playback_peak;
    uint64_t clipped;
    uint64_t playback_clipped;
    h2_pal_result_t diagnostic_rc;
} h2_aec_calibration_measurement_t;

typedef struct h2_aec_calibration_candidate {
    h2_aec_calibration_pair_t requested;
    h2_aec_calibration_pair_t actual;
    h2_pal_result_t result;
    bool pareto;
    h2_aec_calibration_measurement_t measurements
        [H2_AEC_CALIBRATION_LEVELS][H2_AEC_CALIBRATION_PHASE_COUNT];
} h2_aec_calibration_candidate_t;

/** Caller-owned limits; ratio units are parts per thousand of power.
 * Probe qualification is not absolute SPL or unrestricted voice qualification. */
typedef struct h2_aec_calibration_limits {
    uint32_t warmup_frames;
    uint32_t measurement_frames;
    uint32_t stability_frames;
    uint32_t io_timeout_ms;
    uint32_t source_timeout_ms;
    uint32_t max_cadence_milli;
    uint32_t cadence_margin_ms;
    uint32_t max_echo_residual_milli;
    uint32_t min_near_retention_milli;
    uint32_t min_double_talk_retention_milli;
    uint32_t min_reference_power;
    uint32_t min_signal_noise_ratio;
    uint32_t peak_limit;
    uint16_t amplitude[H2_AEC_CALIBRATION_LEVELS];
} h2_aec_calibration_limits_t;

/** Configure an independent real acoustic source. enable=true starts a loop of
 * the supplied mono S16LE probe at a fixed documented position/level; it must
 * never inject PCM into DUT capture or use the DUT speaker. Success confirms
 * the source state. enable=false stops it and is idempotent; failure retains
 * the borrowed probe/user until a successful retry. Called from runner task,
 * with all DUT workers stopped. No independent source means UNSUPPORTED.
 * The source must keep the same geometry/level within a candidate's phases and
 * honor the finite timeout_ms. Runner also rejects a late completed callback;
 * it cannot preempt a callback that violates its bounded-call contract. */
typedef h2_pal_result_t (*h2_aec_calibration_near_source_fn)(
    void *user, bool enable, const h2_audio_pcm_format_t *format,
    const int16_t *probe, size_t samples, uint32_t timeout_ms);

typedef struct h2_aec_calibration_config {
    const h2_pal_audio_api_t *audio;
    const h2_pal_mem_api_t *mem;
    /** Borrowed nonblocking, concurrent monotonic getter; observers timestamp
     * their final capture/DAC frame on the corresponding provider worker. */
    const h2_pal_time_api_t *time;
    const h2_aec_calibration_pair_t *candidates;
    size_t candidate_count;
    h2_aec_calibration_selection_t selection;
    h2_aec_calibration_limits_t limits;
    h2_aec_calibration_near_source_fn near_source;
    void *near_source_user;
} h2_aec_calibration_config_t;

typedef struct h2_aec_calibration_result {
    h2_audio_pcm_format_t format;
    h2_aec_calibration_limits_t limits;
    h2_aec_calibration_selection_t selection;
    uint16_t far_bins[H2_AEC_CALIBRATION_BANDS];
    uint16_t near_bins[H2_AEC_CALIBRATION_BANDS];
    size_t count;
    size_t passed;
    size_t pareto_count;
    size_t selected_index;
    bool selected;
    bool complete;
    bool retained;
    h2_pal_result_t execution_rc;
    h2_pal_result_t cleanup_rc;
    h2_aec_calibration_candidate_t candidates[H2_AEC_CALIBRATION_MAX_CANDIDATES];
} h2_aec_calibration_result_t;

typedef struct h2_aec_calibration h2_aec_calibration_t;

/** Conservative probe defaults: two amplitudes and three distinct near/far
 * speech-band frequencies. Frequencies are bin * sample_rate / frame_samples.
 * Callers must record fixture identity/geometry and these limits with results. */
h2_aec_calibration_limits_t h2_aec_calibration_default_limits(void);

/** Allocate one runner through Memory PAL, borrowing providers/source/user
 * through successful destroy. Candidates and limits are copied. No device I/O.
 * A missing near_source is accepted at create, but run returns UNSUPPORTED. */
h2_pal_result_t h2_aec_calibration_create(const h2_aec_calibration_config_t *config,
                                         h2_aec_calibration_t **out);

/** Blocking serialized run, starting from exclusive stopped DUT Audio. Restores
 * original controls and stops all resources. All candidates run unless a PAL,
 * source or cleanup error prevents trustworthy continuation. Returned report
 * is borrowed from runner until next run/destroy; copy it before destroy.
 * complete means every candidate was measured, not that any candidate passed.
 * Selected values are recommendations for this probe/fixture only, never
 * automatically applied or persisted. retained requires cleanup retry while
 * preserving runner and every borrowed dependency. */
h2_pal_result_t h2_aec_calibration_run(h2_aec_calibration_t *runner,
                                      const h2_aec_calibration_result_t **out);

/** Retry terminal cleanup. Failure retains the instance and all dependencies. */
h2_pal_result_t h2_aec_calibration_cleanup(h2_aec_calibration_t *runner);
/** Idempotent for NULL. Frees only after successful cleanup; failure retains. */
h2_pal_result_t h2_aec_calibration_destroy(h2_aec_calibration_t *runner);

typedef h2_pal_result_t (*h2_aec_calibration_emit_fn)(
    void *user, const char *line, size_t length);
/** Emit bounded JSON records without newlines, with one caller-supplied run
 * identity. Callback is synchronous task-context and must consume the borrowed
 * line before returning. Errors/truncation stop reporting immediately. The
 * caller binds this run to device/image, source fixture identity and geometry;
 * reports alone do not prove physical provenance. */
h2_pal_result_t h2_aec_calibration_report(const h2_aec_calibration_result_t *result,
                                          uint64_t run, h2_aec_calibration_emit_fn emit,
                                          void *user);

#ifdef __cplusplus
}
#endif
#endif
