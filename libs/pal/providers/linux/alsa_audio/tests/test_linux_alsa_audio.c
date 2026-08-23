#include "h2_linux_alsa_abi.h"
#include "h2_linux_alsa_audio.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

struct _snd_pcm {
    size_t written_frames;
};

static struct _snd_pcm s_pcm;
static int s_open_count;
static int s_close_count;
static int s_write_count;
static int s_wait_count;
static int s_recover_count;

static int fake_pcm_open(
    snd_pcm_t **pcm,
    const char *name,
    int stream,
    int mode) {
    assert(strcmp(name, "hw:0,0") == 0);
    assert(stream == H2_SND_PCM_STREAM_PLAYBACK);
    assert(mode == H2_SND_PCM_NONBLOCK);
    ++s_open_count;
    *pcm = &s_pcm;
    return 0;
}

static int fake_pcm_set_params(
    snd_pcm_t *pcm,
    int format,
    int access,
    unsigned int channels,
    unsigned int rate,
    int soft_resample,
    unsigned int latency) {
    assert(pcm == &s_pcm);
    assert(format == H2_SND_PCM_FORMAT_S16_LE);
    assert(access == H2_SND_PCM_ACCESS_RW_INTERLEAVED);
    assert(channels == 1u && rate == 16000u && soft_resample == 0);
    assert(latency >= 20000u && latency <= 500000u);
    return 0;
}

static snd_pcm_sframes_t fake_pcm_writei(
    snd_pcm_t *pcm,
    const void *buffer,
    snd_pcm_uframes_t frames) {
    assert(pcm == &s_pcm && buffer != NULL && frames > 0u);
    ++s_write_count;
    if (s_write_count == 1) return -EAGAIN;
    if (s_write_count == 2) return 0;
    if (s_write_count == 3) return -EPIPE;
    snd_pcm_uframes_t written = frames > 100u ? 100u : frames;
    pcm->written_frames += written;
    return (snd_pcm_sframes_t)written;
}

static int fake_pcm_wait(snd_pcm_t *pcm, int timeout_ms) {
    assert(pcm == &s_pcm && timeout_ms > 0);
    ++s_wait_count;
    if (s_wait_count == 2) return -EPIPE;
    return 1;
}

static int fake_pcm_recover(snd_pcm_t *pcm, int error, int silent) {
    assert(pcm == &s_pcm && error < 0 && silent == 1);
    ++s_recover_count;
    return 0;
}

static int fake_pcm_drop(snd_pcm_t *pcm) {
    assert(pcm == &s_pcm);
    return 0;
}

static int fake_pcm_close(snd_pcm_t *pcm) {
    assert(pcm == &s_pcm);
    ++s_close_count;
    return 0;
}

int h2_linux_alsa_test_load_symbols(h2_linux_alsa_symbols_t *out_symbols) {
    *out_symbols = (h2_linux_alsa_symbols_t){
        .pcm_open = fake_pcm_open,
        .pcm_set_params = fake_pcm_set_params,
        .pcm_writei = fake_pcm_writei,
        .pcm_wait = fake_pcm_wait,
        .pcm_recover = fake_pcm_recover,
        .pcm_drop = fake_pcm_drop,
        .pcm_close = fake_pcm_close,
    };
    return H2_AUDIO_OK;
}

int main(void) {
    const h2_linux_alsa_audio_config_t invalid = {0};
    assert(h2_linux_alsa_audio_configure(&invalid) == H2_PAL_ERR_INVALID_ARG);
    const h2_linux_alsa_audio_config_t config = {
        .device = "hw:0,0",
        .playback_format = {
            .sample_rate_hz = 16000u,
            .frame_samples_per_channel = 320u,
            .channels = 1u,
            .sample_format = H2_AUDIO_SAMPLE_S16LE,
        },
    };
    assert(h2_linux_alsa_audio_configure(&config) == H2_PAL_OK);
    h2_pal_audio_t *audio = h2_linux_alsa_audio_api();
    h2_audio_info_t info = {0};
    assert(h2_pal_audio_get_info(audio, &info) == H2_AUDIO_OK);
    assert(info.available && info.playback_supported && !info.mic_supported);
    assert(info.playback_format.sample_rate_hz == 16000u);
    assert(h2_pal_audio_start_speaker(audio) == H2_AUDIO_OK);
    const h2_audio_track_config_t track_config = {
        .name = "test",
        .format = config.playback_format,
        .volume_factor_milli = 1000u,
        .buffer_frames = 8u,
    };
    h2_pal_audio_track_t *track = NULL;
    assert(h2_pal_audio_create_track(audio, &track_config, &track) == H2_AUDIO_OK);
    assert(track != NULL && s_open_count == 1);
    int16_t samples[320];
    for (size_t i = 0u; i < 320u; ++i) samples[i] = (int16_t)i;
    const h2_audio_frame_t frame = {
        .data = samples,
        .capacity = sizeof(samples),
        .bytes = sizeof(samples),
        .sample_rate_hz = 16000u,
        .samples_per_channel = 320u,
        .channels = 1u,
        .sample_format = H2_AUDIO_SAMPLE_S16LE,
    };
    h2_audio_frame_t oversized_frame = frame;
    oversized_frame.capacity -= sizeof(samples[0]);
    assert(h2_pal_audio_track_write(track, &oversized_frame, 100u) ==
        H2_AUDIO_ERR_INVALID_ARG);
    assert(s_write_count == 0);
    assert(h2_pal_audio_track_write(track, &frame, 100u) == H2_AUDIO_OK);
    assert(s_pcm.written_frames == 320u && s_write_count >= 6);
    assert(s_wait_count == 2 && s_recover_count == 2);
    assert(h2_pal_audio_set_speaker_volume_percent(audio, 60u) == H2_AUDIO_OK);
    assert(h2_pal_audio_track_set_volume_factor(track, 500u) == H2_AUDIO_OK);
    uint32_t factor = 0u;
    assert(h2_pal_audio_track_get_volume_factor(track, &factor) == H2_AUDIO_OK);
    assert(factor == 500u);
    assert(h2_pal_audio_track_close(track) == H2_AUDIO_OK);
    assert(s_close_count == 1);
    assert(h2_pal_audio_stop_speaker(audio) == H2_AUDIO_OK);
    return 0;
}
