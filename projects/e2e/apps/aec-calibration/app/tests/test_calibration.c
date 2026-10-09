#include "h2_aec_calibration.h"
#include "h2/pal/h2_pal_unsupported.h"

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct fixture {
    h2_pal_audio_api_t audio;
    h2_pal_audio_track_t track;
    h2_audio_aec_observer_t observer;
    h2_pal_time_api_t time;
    uint32_t volume, gain;
    uint64_t now, sequence;
    bool mic, speaker, near;
    bool mute, mute_double, no_aec, no_reference, malformed, fast_clock, slow_clock;
    bool fail_stop, fail_source_stop, fail_unregister;
    bool clip_input_reference;
    bool clip_second_mic;
    bool dac_error;
    bool slow_source;
    bool drop_playback, zero_playback, moderately_fast;
    unsigned writes;
    unsigned allocations, frees, near_starts;
    int16_t playback[512];
    const int16_t *near_probe;
} fixture_t;

static void *allocate(void *u, size_t bytes) {
    fixture_t *f = u;
    ++f->allocations;
    return malloc(bytes);
}
static void release(void *u, void *ptr) {
    fixture_t *f = u;
    ++f->frees;
    free(ptr);
}
static int now(void *u, uint64_t *value) {
    *value = ((fixture_t *)u)->now;
    return H2_PAL_OK;
}
static h2_audio_pcm_format_t format(void) {
    return (h2_audio_pcm_format_t){16000u, 512u, 1u, H2_AUDIO_SAMPLE_S16LE};
}
static int info(void *u, h2_audio_info_t *out) {
    (void)u;
    *out = (h2_audio_info_t){.available = 1, .mic_supported = 1,
        .playback_supported = 1, .mic_format = format(), .playback_format = format(),
        .max_tracks = 1, .mic_queue_frames = 4, .track_queue_frames = 4};
    return H2_AUDIO_OK;
}
static int mic_start(void *u) {
    fixture_t *f = u;
    f->mic = true;
    f->sequence = 0u;
    f->writes = 0u;
    return H2_AUDIO_OK;
}
static int mic_stop(void *u) {
    fixture_t *f = u;
    if (f->fail_stop)
        return H2_AUDIO_ERR_IO;
    f->mic = false;
    return H2_AUDIO_OK;
}
static int speaker_start(void *u) {
    ((fixture_t *)u)->speaker = true;
    return H2_AUDIO_OK;
}
static int speaker_stop(void *u) {
    ((fixture_t *)u)->speaker = false;
    return H2_AUDIO_OK;
}
static int get_volume(void *u, uint32_t *out) {
    *out = ((fixture_t *)u)->volume;
    return H2_AUDIO_OK;
}
static int set_volume(void *u, uint32_t value) {
    ((fixture_t *)u)->volume = value / 10u * 10u;
    return H2_AUDIO_OK;
}
static int get_gain(void *u, uint32_t *out) {
    *out = ((fixture_t *)u)->gain;
    return H2_AUDIO_OK;
}
static int set_gain(void *u, uint32_t value) {
    ((fixture_t *)u)->gain = value / 10u * 10u;
    return H2_AUDIO_OK;
}
static int observe(void *u, const h2_audio_aec_observer_t *observer) {
    fixture_t *f = u;
    if (f->mic || f->speaker)
        return H2_AUDIO_ERR_INVALID_STATE;
    if (observer == NULL && f->fail_unregister)
        return H2_AUDIO_ERR_IO;
    f->observer = observer != NULL ? *observer : (h2_audio_aec_observer_t){0};
    return H2_AUDIO_OK;
}
static int write_track(h2_pal_audio_track_t *track, const h2_audio_frame_t *frame,
                        uint32_t timeout) {
    (void)timeout;
    fixture_t *f = track->user;
    ++f->writes;
    const bool alternate_measured = f->writes > 4u && f->writes % 2u == 0u;
    if (f->zero_playback && alternate_measured)
        memset(f->playback, 0, sizeof(f->playback));
    else
        memcpy(f->playback, frame->data, sizeof(f->playback));
    h2_audio_frame_t observed = *frame;
    observed.data = f->playback;
    if (f->observer.on_playback != NULL && !(f->drop_playback && alternate_measured))
        f->observer.on_playback(f->observer.user, &observed);
    if (f->dac_error && f->observer.on_error != NULL)
        f->observer.on_error(f->observer.user, 0, H2_AUDIO_ERR_IO);
    return H2_AUDIO_OK;
}
static int close_track(h2_pal_audio_track_t *track) {
    (void)track;
    return H2_AUDIO_OK;
}
static int drain(h2_pal_audio_track_t *track, uint32_t timeout) {
    (void)track;
    (void)timeout;
    return H2_AUDIO_OK;
}
static int create_track(void *u, const h2_audio_track_config_t *config,
                         h2_pal_audio_track_t **out) {
    (void)config;
    fixture_t *f = u;
    f->track = (h2_pal_audio_track_t){.user = u, .write = write_track,
                                     .close = close_track, .drain = drain};
    *out = &f->track;
    return H2_AUDIO_OK;
}
static int16_t clamp(int value) {
    if (value > INT16_MAX)
        return INT16_MAX;
    if (value < INT16_MIN)
        return INT16_MIN;
    return (int16_t)value;
}
static int read_mic(void *u, h2_audio_frame_t *out, uint32_t timeout) {
    (void)timeout;
    fixture_t *f = u;
    assert(f->mic && f->speaker);
    int16_t raw[1536], reference[512], processed[512];
    const uint8_t channels = f->clip_second_mic ? 3u : 2u;
    bool playing = false;
    for (size_t i = 0u; i < 512u; ++i)
        playing = playing || f->playback[i] != 0;
    for (size_t i = 0u; i < 512u; ++i) {
        const int far = (int)f->playback[i] * (int)f->volume / 100;
        const int near = f->near ? f->near_probe[i] : 0;
        int mic = (far / 2 + near) * (int)f->gain / 100 + 8;
        if (f->volume == 100u && f->gain == 100u)
            mic *= 4;
        raw[i * channels] = clamp(mic);
        raw[i * channels + 1u] = f->no_reference ? 0 : (int16_t)(far / 2);
        if (f->clip_second_mic)
            raw[i * channels + 2u] = INT16_MAX;
        reference[i] = f->clip_input_reference ? INT16_MAX : raw[i * channels + 1u];
        int value = f->no_aec ? mic : (far / 32 + near * 3 / 4) * (int)f->gain / 100 + 8;
        if (f->mute || (f->mute_double && f->near && playing))
            value = 0;
        processed[i] = clamp(value);
    }
    h2_audio_pcm_format_t raw_format = format();
    raw_format.channels = channels;
    h2_audio_frame_t raw_frame = h2_audio_frame_for_buffer(raw, sizeof(raw), raw_format);
    h2_audio_frame_t ref_frame = h2_audio_frame_for_buffer(reference, sizeof(reference), format());
    h2_audio_frame_t output = h2_audio_frame_for_buffer(processed, sizeof(processed), format());
    raw_frame.bytes = (size_t)channels * 512u * sizeof(int16_t);
    ref_frame.bytes = sizeof(reference);
    output.bytes = sizeof(processed);
    if (f->malformed)
        --raw_frame.bytes;
    const h2_audio_aec_frame_t frame = {.sequence = ++f->sequence,
        .raw = &raw_frame, .reference_input = &ref_frame, .processed = &output,
        .microphone_lane = 0, .reference_lane = 1,
        .microphone_mask = f->clip_second_mic ? 5u : 1u};
    if (f->observer.on_capture != NULL)
        f->observer.on_capture(f->observer.user, &frame);
    memcpy(out->data, processed, sizeof(processed));
    out->bytes = sizeof(processed);
    out->samples_per_channel = 512;
    f->now += f->fast_clock ? 1u : (f->slow_clock ? 96u : (f->moderately_fast ? 24u : 32u));
    return H2_AUDIO_OK;
}
static int near_source(void *u, bool enabled, const h2_audio_pcm_format_t *fmt,
                         const int16_t *probe, size_t samples, uint32_t timeout_ms) {
    fixture_t *f = u;
    assert(fmt->sample_rate_hz == 16000u && samples == 512u);
    assert(timeout_ms == 5000u);
    if (f->slow_source && enabled)
        f->now += 6000u;
    if (!enabled && f->fail_source_stop && f->near)
        return H2_PAL_ERR_IO;
    f->near = enabled;
    f->near_probe = probe;
    if (enabled)
        ++f->near_starts;
    return H2_PAL_OK;
}
static const h2_pal_audio_vtable_t audio_vtable = {
    .get_info = info, .start_mic = mic_start, .stop_mic = mic_stop,
    .start_speaker = speaker_start, .stop_speaker = speaker_stop,
    .mic_read = read_mic, .create_track = create_track,
    .get_speaker_volume_percent = get_volume, .set_speaker_volume_percent = set_volume,
    .get_mic_gain_percent = get_gain, .set_mic_gain_percent = set_gain,
    .set_aec_observer = observe,
};
static const h2_pal_time_vtable_t time_vtable = {.get_monotonic_ms = now};
static const h2_pal_mem_vtable_t mem_vtable = {.alloc = allocate, .free = release};
static const h2_aec_calibration_pair_t pairs[] = {{100, 100}, {100, 60}, {80, 100}, {80, 60}};

static h2_aec_calibration_config_t config(fixture_t *f, h2_pal_mem_api_t *mem) {
    f->volume = 70u;
    f->gain = 60u;
    f->audio = (h2_pal_audio_api_t){f, &audio_vtable};
    f->time = (h2_pal_time_api_t){f, &time_vtable};
    *mem = (h2_pal_mem_api_t){f, &mem_vtable};
    h2_aec_calibration_limits_t limits = h2_aec_calibration_default_limits();
    limits.warmup_frames = 4;
    limits.measurement_frames = 8;
    limits.stability_frames = 16;
    return (h2_aec_calibration_config_t){.audio = &f->audio, .mem = mem,
        .time = &f->time, .candidates = pairs, .candidate_count = 4,
        .limits = limits, .near_source = near_source, .near_source_user = f};
}

static void selection_test(h2_aec_calibration_selection_t policy) {
    fixture_t f = {0};
    h2_pal_mem_api_t mem;
    h2_aec_calibration_config_t c = config(&f, &mem);
    c.selection = policy;
    h2_aec_calibration_t *runner = NULL;
    assert(h2_aec_calibration_create(&c, &runner) == H2_PAL_OK);
    const h2_aec_calibration_result_t *r = NULL;
    assert(h2_aec_calibration_run(runner, &r) == H2_PAL_OK);
    assert(r->complete && !r->retained && r->cleanup_rc == 0);
    assert(r->passed == 3 && r->pareto_count == 2);
    assert(r->candidates[0].result != H2_PAL_OK);
    assert(r->candidates[1].pareto && r->candidates[2].pareto && !r->candidates[3].pareto);
    assert(r->selected == (policy != H2_AEC_CALIBRATION_PARETO_ONLY));
    if (r->selected)
        assert(r->selected_index == (policy == H2_AEC_CALIBRATION_SPEAKER_FIRST ? 1u : 2u));
    assert(f.near_starts == 4u * 2u * 3u);
    assert(f.volume == 70 && f.gain == 60 && !f.mic && !f.speaker && !f.near);
    assert(f.observer.on_capture == NULL);
    assert(h2_aec_calibration_run(runner, &r) == H2_PAL_ERR_INVALID_STATE);
    assert(h2_aec_calibration_destroy(runner) == H2_PAL_OK);
    assert(f.allocations == f.frees);
}

static void rejects_test(unsigned failure) {
    fixture_t f = {0};
    h2_pal_mem_api_t mem;
    h2_aec_calibration_config_t c = config(&f, &mem);
    c.candidates = pairs + 1;
    c.candidate_count = 1;
    switch (failure) {
        case 0: f.mute = true; break;
        case 1: f.mute_double = true; break;
        case 2: f.no_aec = true; break;
        case 3: f.no_reference = true; break;
        case 4: f.malformed = true; break;
        case 5: f.fast_clock = true; break;
        case 6: f.clip_input_reference = true; break;
        case 7: f.slow_clock = true; break;
        case 8: f.clip_second_mic = true; break;
        case 9: f.dac_error = true; break;
        case 10: f.slow_source = true; break;
        case 11: f.drop_playback = true; break;
        case 12: f.zero_playback = true; break;
        case 13:
            f.moderately_fast = true;
            c.limits.stability_frames = c.limits.measurement_frames;
            break;
        default: abort();
    }
    h2_aec_calibration_t *runner = NULL;
    assert(h2_aec_calibration_create(&c, &runner) == H2_PAL_OK);
    const h2_aec_calibration_result_t *r = NULL;
    assert(h2_aec_calibration_run(runner, &r) != H2_PAL_OK);
    assert(!r->selected && r->passed == 0 && !r->retained);
    if (failure == 11u || failure == 13u)
        assert(f.near_starts == 0u);
    assert(h2_aec_calibration_destroy(runner) == H2_PAL_OK);
    assert(f.allocations == f.frees);
}

static void retained_test(unsigned failure) {
    fixture_t f = {0};
    h2_pal_mem_api_t mem;
    h2_aec_calibration_config_t c = config(&f, &mem);
    c.candidates = pairs + 1;
    c.candidate_count = 1;
    f.fail_stop = failure == 0;
    f.fail_source_stop = failure == 1;
    f.fail_unregister = failure == 2;
    h2_aec_calibration_t *runner = NULL;
    const h2_aec_calibration_result_t *r = NULL;
    assert(h2_aec_calibration_create(&c, &runner) == H2_PAL_OK);
    assert(h2_aec_calibration_run(runner, &r) != H2_PAL_OK);
    assert(r->retained && !r->selected);
    if (failure == 1u)
        assert(!f.mic && !f.speaker && f.near);
    assert(h2_aec_calibration_destroy(runner) != H2_PAL_OK);
    assert(f.frees == 0);
    f.fail_stop = f.fail_source_stop = f.fail_unregister = false;
    assert(h2_aec_calibration_destroy(runner) == H2_PAL_OK);
    assert(f.allocations == f.frees && !f.near && !f.mic && !f.speaker);
}

static void unsupported_and_validation_test(void) {
    fixture_t f = {0};
    h2_pal_mem_api_t mem;
    h2_aec_calibration_config_t c = config(&f, &mem);
    const h2_audio_aec_observer_t invalid = {0};
    assert(h2_pal_audio_set_aec_observer(NULL, NULL) == H2_AUDIO_ERR_INVALID_ARG);
    assert(h2_pal_audio_set_aec_observer(c.audio, &invalid) == H2_AUDIO_ERR_INVALID_ARG);
    h2_aec_calibration_t *runner = NULL;
    const h2_aec_calibration_result_t *r = NULL;
    c.near_source = NULL;
    assert(h2_aec_calibration_create(&c, &runner) == H2_PAL_OK);
    assert(h2_aec_calibration_run(runner, &r) == H2_PAL_ERR_UNSUPPORTED);
    assert(!r->complete && !r->selected && f.sequence == 0);
    assert(h2_aec_calibration_destroy(runner) == H2_PAL_OK);
    c.near_source = near_source;
    c.audio = h2_pal_unsupported_audio_api();
    assert(h2_pal_audio_set_aec_observer(c.audio, NULL) == H2_AUDIO_ERR_UNSUPPORTED);
    assert(h2_aec_calibration_create(&c, &runner) == H2_PAL_OK);
    assert(h2_aec_calibration_run(runner, &r) == H2_PAL_ERR_UNSUPPORTED);
    assert(h2_aec_calibration_destroy(runner) == H2_PAL_OK);
    c = config(&f, &mem);
    const h2_aec_calibration_pair_t minimum_gain[] = {{100, 0}};
    c.candidates = minimum_gain;
    c.candidate_count = 1u;
    assert(h2_aec_calibration_create(&c, &runner) == H2_PAL_OK);
    assert(h2_aec_calibration_run(runner, &r) != H2_PAL_OK);
    assert(r->candidates[0].actual.mic_percent == 0u && r->complete);
    assert(h2_aec_calibration_destroy(runner) == H2_PAL_OK);
    c = config(&f, &mem);
    c.limits.amplitude[1] = 32767;
    assert(h2_aec_calibration_create(&c, &runner) == H2_PAL_ERR_INVALID_ARG && runner == NULL);
    c = config(&f, &mem);
    const h2_aec_calibration_pair_t repeated[] = {{100, 50}, {100, 50}};
    c.candidates = repeated;
    c.candidate_count = 2;
    assert(h2_aec_calibration_create(&c, &runner) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_aec_calibration_destroy(NULL) == H2_PAL_OK);
}

static int print_record(void *user, const char *line, size_t length) {
    (void)user;
    assert(fwrite(line, 1u, length, stdout) == length);
    assert(fputc('\n', stdout) != EOF);
    return H2_PAL_OK;
}
static int reject_record(void *user, const char *line, size_t length) {
    (void)user; (void)line; (void)length;
    return H2_PAL_ERR_IO;
}
int main(int argc, char **argv) {
    if (argc == 2 && (strcmp(argv[1], "--report") == 0 ||
                      strcmp(argv[1], "--unsupported-report") == 0)) {
        fixture_t f = {0};
        h2_pal_mem_api_t mem;
        h2_aec_calibration_config_t c = config(&f, &mem);
        const bool unsupported = strcmp(argv[1], "--unsupported-report") == 0;
        if (unsupported)
            c.audio = h2_pal_unsupported_audio_api();
        h2_aec_calibration_t *runner = NULL;
        const h2_aec_calibration_result_t *result = NULL;
        assert(h2_aec_calibration_create(&c, &runner) == H2_PAL_OK);
        assert(h2_aec_calibration_run(runner, &result) ==
               (unsupported ? H2_PAL_ERR_UNSUPPORTED : H2_PAL_OK));
        assert(h2_aec_calibration_report(result, 1u, reject_record, NULL) == H2_PAL_ERR_IO);
        assert(h2_aec_calibration_report(result, 1u, print_record, NULL) == H2_PAL_OK);
        assert(h2_aec_calibration_destroy(runner) == H2_PAL_OK);
        return 0;
    }
    selection_test(H2_AEC_CALIBRATION_PARETO_ONLY);
    selection_test(H2_AEC_CALIBRATION_SPEAKER_FIRST);
    selection_test(H2_AEC_CALIBRATION_MIC_FIRST);
    for (unsigned i = 0u; i < 14u; ++i)
        rejects_test(i);
    for (unsigned i = 0u; i < 3u; ++i)
        retained_test(i);
    unsupported_and_validation_test();
    return 0;
}
