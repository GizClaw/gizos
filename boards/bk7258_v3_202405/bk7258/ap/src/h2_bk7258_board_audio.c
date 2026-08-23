#include "h2_bk7258_board_private.h"

#include "h2_audio_mixer.h"

#include <common/bk_include.h>
#include <components/log.h>
#include <os/mem.h>
#include <os/os.h>

#include "audio_play.h"
#include "audio_record.h"
#include "audio_osi_wrapper.h"
#include "modules/aec.h"

#include <stdbool.h>
#include <string.h>

#define TAG "h2_bk_audio"
#define H2_BK_AUDIO_MAX_TRACKS 4u
#define H2_BK_AUDIO_FRAME_SAMPLES 320u
#define H2_BK_AUDIO_RAW_MIC_SAMPLES (H2_BK_AUDIO_FRAME_SAMPLES * 2u)
#define H2_BK_AUDIO_PLAYBACK_SCRATCH_SAMPLES 1024u
#define H2_BK_AUDIO_TRACK_QUEUE_FRAMES 4u
#define H2_BK_AUDIO_MIC_QUEUE_FRAMES 4u

#define H2_BK_AEC_DELAY_SAMPLES 211u
#define H2_BK_AEC_EC_DEPTH 50u
#define H2_BK_AEC_TX_RX_THR 30u
#define H2_BK_AEC_TX_RX_FLR 6u
#define H2_BK_AEC_REF_SCALE 0u
#define H2_BK_AEC_NS_LEVEL 2u
#define H2_BK_AEC_NS_PARA 1u
#define H2_BK_AEC_VOICE_VOLUME 8u
#define H2_BK_AEC_DRC 0x10u

typedef struct h2_bk_mic_queue_frame {
    size_t bytes;
    uint16_t samples_per_channel;
    int16_t samples[H2_BK_AUDIO_FRAME_SAMPLES];
} h2_bk_mic_queue_frame_t;

typedef struct h2_bk_audio_state {
    bool opened;
    bool speaker_initialized;
    bool speaker_started;
    bool speaker_open;
    bool mic_initialized;
    bool mic_record_opened;
    bool mic_open;
    volatile bool mic_reading;
    bool mic_queue_initialized;
    bool mic_thread_started;
    bool ref_mutex_initialized;
    bool aec_initialized;
    bool mixer_initialized;
    bool playback_thread_started;
    uint32_t speaker_volume_percent;
    audio_play_t *play;
    audio_record_t *record;
    AECContext *aec;
    int16_t *aec_ref;
    int16_t *aec_mic;
    int16_t *aec_out;
    h2_audio_mixer_t mixer;
    h2_pal_queue_t *mic_queue;
    beken_thread_t playback_thread;
    beken_thread_t mic_thread;
    beken_mutex_t ref_mutex;
    int16_t latest_ref[H2_BK_AUDIO_FRAME_SAMPLES];
    int16_t mic_raw_scratch[H2_BK_AUDIO_RAW_MIC_SAMPLES];
    int16_t mic_mono_scratch[H2_BK_AUDIO_FRAME_SAMPLES];
    int16_t mic_processed_scratch[H2_BK_AUDIO_FRAME_SAMPLES];
    int16_t aec_ref_scratch[H2_BK_AUDIO_FRAME_SAMPLES];
    int16_t ref_scratch[H2_BK_AUDIO_PLAYBACK_SCRATCH_SAMPLES];
    int16_t playback_scratch[H2_BK_AUDIO_PLAYBACK_SCRATCH_SAMPLES];
} h2_bk_audio_state_t;

static h2_bk_audio_state_t s_audio_state = {
    .speaker_volume_percent = 100u,
};

static h2_audio_pcm_format_t bk_mic_format(void) {
    h2_audio_pcm_format_t format = {
        .sample_rate_hz = h2_bk7258_audio_config.sample_rate,
        .frame_samples_per_channel = h2_bk7258_audio_config.frame_samples_per_channel,
        .channels = 1u,
        .sample_format = H2_AUDIO_SAMPLE_S16LE,
    };
    return format;
}

static h2_audio_pcm_format_t bk_raw_mic_format(void) {
    h2_audio_pcm_format_t format = {
        .sample_rate_hz = h2_bk7258_audio_config.sample_rate,
        .frame_samples_per_channel = h2_bk7258_audio_config.frame_samples_per_channel,
        .channels = h2_bk7258_audio_config.mic_channels,
        .sample_format = H2_AUDIO_SAMPLE_S16LE,
    };
    return format;
}

static h2_audio_pcm_format_t bk_playback_format(void) {
    h2_audio_pcm_format_t format = {
        .sample_rate_hz = h2_bk7258_audio_config.sample_rate,
        .frame_samples_per_channel = h2_bk7258_audio_config.frame_samples_per_channel,
        .channels = h2_bk7258_audio_config.speaker_channels,
        .sample_format = H2_AUDIO_SAMPLE_S16LE,
    };
    return format;
}

static int map_bk_rc(int rc) {
    if (rc == BK_OK) {
        return H2_AUDIO_OK;
    }
    return H2_AUDIO_ERR_IO;
}

static int bk_volume_from_percent(uint32_t percent) {
    if (percent > 100u) {
        percent = 100u;
    }
    return (int)((percent * h2_bk7258_audio_config.default_volume) / 100u);
}

static int bk_audio_init_mixer(h2_bk_audio_state_t *state) {
    if (state->mixer_initialized) {
        return H2_AUDIO_OK;
    }
    h2_audio_mixer_config_t config = {
        .format = bk_playback_format(),
        .max_tracks = H2_BK_AUDIO_MAX_TRACKS,
        .track_queue_frames = H2_BK_AUDIO_TRACK_QUEUE_FRAMES,
        .allocator = h2_bk7258_board_default_allocator(),
        .queue_api = h2_bk7258_board_queue_api(),
        .sync_api = h2_bk7258_board_sync_api(),
    };
    int rc = h2_audio_mixer_init(&state->mixer, &config);
    if (rc != H2_AUDIO_OK) {
        return rc;
    }
    state->mixer_initialized = true;
    return H2_AUDIO_OK;
}

static int bk_audio_init_ref_mutex(h2_bk_audio_state_t *state) {
    if (state->ref_mutex_initialized) {
        return H2_AUDIO_OK;
    }
    if (rtos_init_mutex(&state->ref_mutex) != kNoErr) {
        return H2_AUDIO_ERR_IO;
    }
    state->ref_mutex_initialized = true;
    return H2_AUDIO_OK;
}

static int bk_audio_init_mic_queue(h2_bk_audio_state_t *state) {
    if (state->mic_queue_initialized) {
        return H2_AUDIO_OK;
    }
    h2_pal_queue_config_t config = {
        .name = "bk_mic",
        .item_size = sizeof(h2_bk_mic_queue_frame_t),
        .item_count = H2_BK_AUDIO_MIC_QUEUE_FRAMES,
        .allocator = h2_bk7258_board_default_allocator(),
    };
    int rc = h2_pal_queue_create(h2_bk7258_board_queue_api(), &config, &state->mic_queue);
    if (rc != H2_PAL_QUEUE_OK) {
        return rc == H2_PAL_QUEUE_ERR_NO_MEMORY ? H2_AUDIO_ERR_NO_MEMORY : H2_AUDIO_ERR_IO;
    }
    state->mic_queue_initialized = true;
    return H2_AUDIO_OK;
}

static int bk_audio_init_aec(h2_bk_audio_state_t *state) {
    if (state->aec_initialized) {
        return H2_AUDIO_OK;
    }
    if (h2_bk7258_audio_config.sample_rate != 8000u && h2_bk7258_audio_config.sample_rate != 16000u) {
        return H2_AUDIO_ERR_UNSUPPORTED;
    }
    if (h2_bk7258_audio_config.frame_samples_per_channel != H2_BK_AUDIO_FRAME_SAMPLES ||
        h2_bk7258_audio_config.speaker_channels != 1u) {
        return H2_AUDIO_ERR_UNSUPPORTED;
    }
    if (bk_audio_osi_funcs_init() != BK_OK) {
        return H2_AUDIO_ERR_IO;
    }

    uint32_t context_size = aec_size(1000u);
    state->aec = (AECContext *)os_malloc(context_size);
    if (state->aec == NULL) {
        return H2_AUDIO_ERR_NO_MEMORY;
    }
    os_memset(state->aec, 0, context_size);
    aec_init(state->aec, (int16_t)h2_bk7258_audio_config.sample_rate);

    uint32_t actual_frame_samples = 0u;
    uint32_t value = 0u;
    aec_ctrl(state->aec, AEC_CTRL_CMD_GET_FRAME_SAMPLE, (uint32_t)(uintptr_t)&actual_frame_samples);
    if (actual_frame_samples != h2_bk7258_audio_config.frame_samples_per_channel) {
        BK_LOGE(TAG, "aec frame mismatch actual=%u expected=%u\r\n",
            actual_frame_samples,
            (unsigned)h2_bk7258_audio_config.frame_samples_per_channel);
        os_free(state->aec);
        state->aec = NULL;
        return H2_AUDIO_ERR_UNSUPPORTED;
    }

    aec_ctrl(state->aec, AEC_CTRL_CMD_GET_RX_BUF, (uint32_t)(uintptr_t)&value);
    state->aec_ref = (int16_t *)(uintptr_t)value;
    aec_ctrl(state->aec, AEC_CTRL_CMD_GET_TX_BUF, (uint32_t)(uintptr_t)&value);
    state->aec_mic = (int16_t *)(uintptr_t)value;
    aec_ctrl(state->aec, AEC_CTRL_CMD_GET_OUT_BUF, (uint32_t)(uintptr_t)&value);
    state->aec_out = (int16_t *)(uintptr_t)value;
    if (state->aec_ref == NULL || state->aec_mic == NULL || state->aec_out == NULL) {
        os_free(state->aec);
        state->aec = NULL;
        state->aec_ref = NULL;
        state->aec_mic = NULL;
        state->aec_out = NULL;
        return H2_AUDIO_ERR_IO;
    }

    aec_ctrl(state->aec, AEC_CTRL_CMD_SET_FLAGS, 0x1fu);
    aec_ctrl(state->aec, AEC_CTRL_CMD_SET_MIC_DELAY, H2_BK_AEC_DELAY_SAMPLES);
    aec_ctrl(state->aec, AEC_CTRL_CMD_SET_EC_DEPTH, H2_BK_AEC_EC_DEPTH);
    aec_ctrl(state->aec, AEC_CTRL_CMD_SET_TxRxThr, H2_BK_AEC_TX_RX_THR);
    aec_ctrl(state->aec, AEC_CTRL_CMD_SET_TxRxFlr, H2_BK_AEC_TX_RX_FLR);
    aec_ctrl(state->aec, AEC_CTRL_CMD_SET_REF_SCALE, H2_BK_AEC_REF_SCALE);
    aec_ctrl(state->aec, AEC_CTRL_CMD_SET_VOL, H2_BK_AEC_VOICE_VOLUME);
    aec_ctrl(state->aec, AEC_CTRL_CMD_SET_NS_LEVEL, H2_BK_AEC_NS_LEVEL);
    aec_ctrl(state->aec, AEC_CTRL_CMD_SET_NS_PARA, H2_BK_AEC_NS_PARA);
    aec_ctrl(state->aec, AEC_CTRL_CMD_SET_DRC, H2_BK_AEC_DRC);

    state->aec_initialized = true;
    return bk_audio_init_ref_mutex(state);
}

static void bk_audio_copy_latest_ref(h2_bk_audio_state_t *state, int16_t *out, size_t samples) {
    if (samples > H2_BK_AUDIO_FRAME_SAMPLES) {
        samples = H2_BK_AUDIO_FRAME_SAMPLES;
    }
    if (state->ref_mutex_initialized) {
        rtos_lock_mutex(&state->ref_mutex);
    }
    os_memcpy(out, state->latest_ref, samples * sizeof(int16_t));
    if (state->ref_mutex_initialized) {
        rtos_unlock_mutex(&state->ref_mutex);
    }
}

static void bk_audio_store_latest_ref(h2_bk_audio_state_t *state, const int16_t *samples, size_t count) {
    if (count > H2_BK_AUDIO_FRAME_SAMPLES) {
        count = H2_BK_AUDIO_FRAME_SAMPLES;
    }
    if (state->ref_mutex_initialized) {
        rtos_lock_mutex(&state->ref_mutex);
    }
    os_memset(state->latest_ref, 0, sizeof(state->latest_ref));
    if (samples != NULL && count > 0u) {
        os_memcpy(state->latest_ref, samples, count * sizeof(int16_t));
    }
    if (state->ref_mutex_initialized) {
        rtos_unlock_mutex(&state->ref_mutex);
    }
}

static void bk_audio_downmix_mic(const int16_t *raw, int16_t *mono, uint16_t frames, uint8_t channels) {
    if (channels <= 1u) {
        os_memcpy(mono, raw, (size_t)frames * sizeof(int16_t));
        return;
    }
    for (uint16_t frame = 0u; frame < frames; ++frame) {
        int32_t sum = 0;
        for (uint8_t ch = 0u; ch < channels; ++ch) {
            sum += raw[((size_t)frame * channels) + ch];
        }
        mono[frame] = (int16_t)(sum / channels);
    }
}

static int bk_audio_process_mic(h2_bk_audio_state_t *state, const int16_t *raw, uint16_t frames) {
    if (!state->aec_initialized || state->aec == NULL ||
        state->aec_ref == NULL || state->aec_mic == NULL || state->aec_out == NULL ||
        frames != H2_BK_AUDIO_FRAME_SAMPLES) {
        return H2_AUDIO_ERR_INVALID_STATE;
    }
    bk_audio_downmix_mic(raw, state->mic_mono_scratch, frames, h2_bk7258_audio_config.mic_channels);
    bk_audio_copy_latest_ref(state, state->aec_ref_scratch, frames);
    os_memcpy(state->aec_ref, state->aec_ref_scratch, (size_t)frames * sizeof(int16_t));
    os_memcpy(state->aec_mic, state->mic_mono_scratch, (size_t)frames * sizeof(int16_t));
    aec_proc(state->aec, state->aec_ref, state->aec_mic, state->aec_out);
    os_memcpy(state->mic_processed_scratch, state->aec_out, (size_t)frames * sizeof(int16_t));
    return H2_AUDIO_OK;
}

static int map_queue_recv_rc(int rc) {
    if (rc == H2_PAL_QUEUE_OK) {
        return H2_AUDIO_OK;
    }
    if (rc == H2_PAL_QUEUE_ERR_TIMEOUT) {
        return H2_AUDIO_ERR_WOULD_BLOCK;
    }
    if (rc == H2_PAL_QUEUE_ERR_CLOSED) {
        return H2_AUDIO_ERR_INVALID_STATE;
    }
    if (rc == H2_PAL_QUEUE_ERR_INVALID_ARG) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    return H2_AUDIO_ERR_IO;
}

static void bk_audio_playback_task(void *arg) {
    h2_bk_audio_state_t *state = (h2_bk_audio_state_t *)arg;
    for (;;) {
        if (!state->speaker_open || state->play == NULL || !state->mixer_initialized) {
            rtos_delay_milliseconds(20);
            continue;
        }
        h2_audio_frame_t frame = h2_audio_frame_for_buffer(
            state->playback_scratch,
            sizeof(state->playback_scratch),
            bk_playback_format());
        h2_audio_frame_t ref_frame = h2_audio_frame_for_buffer(
            state->ref_scratch,
            sizeof(state->ref_scratch),
            bk_playback_format());
        int rc = h2_audio_mixer_read_with_reference(&state->mixer, &frame, &ref_frame);
        if (rc == H2_AUDIO_OK && frame.bytes > 0u) {
            bk_audio_store_latest_ref(state, (const int16_t *)ref_frame.data, ref_frame.bytes / sizeof(int16_t));
            int written = audio_play_write_data(state->play, (char *)state->playback_scratch, (uint32_t)frame.bytes);
            if (written < 0) {
                BK_LOGE(TAG, "audio_play_write_data failed=%d\r\n", written);
                rtos_delay_milliseconds(20);
            }
        } else {
            rtos_delay_milliseconds(20);
        }
    }
}

static void bk_audio_mic_task(void *arg) {
    h2_bk_audio_state_t *state = (h2_bk_audio_state_t *)arg;
    const h2_audio_pcm_format_t raw_format = bk_raw_mic_format();
    const size_t raw_frame_bytes = h2_audio_pcm_frame_bytes(&raw_format);
    const size_t wanted = (size_t)raw_format.frame_samples_per_channel * raw_frame_bytes;

    for (;;) {
        if (!state->mic_open || state->record == NULL || state->mic_queue == NULL) {
            rtos_delay_milliseconds(20);
            continue;
        }
        state->mic_reading = true;
        int read = audio_record_read_data(state->record, (char *)state->mic_raw_scratch, (uint32_t)wanted);
        state->mic_reading = false;
        if (!state->mic_open) {
            continue;
        }
        if (read <= 0 || (size_t)read != wanted) {
            rtos_delay_milliseconds(20);
            continue;
        }
        int rc = bk_audio_process_mic(state, state->mic_raw_scratch, raw_format.frame_samples_per_channel);
        if (rc != H2_AUDIO_OK) {
            BK_LOGE(TAG, "mic process failed rc=%d\r\n", rc);
            rtos_delay_milliseconds(20);
            continue;
        }
        h2_bk_mic_queue_frame_t item;
        item.bytes = (size_t)raw_format.frame_samples_per_channel * sizeof(int16_t);
        item.samples_per_channel = raw_format.frame_samples_per_channel;
        os_memcpy(item.samples, state->mic_processed_scratch, item.bytes);
        (void)h2_pal_queue_send_latest(h2_bk7258_board_queue_api(), state->mic_queue, &item);
    }
}

static int bk_audio_start_mic_task(h2_bk_audio_state_t *state) {
    if (state->mic_thread_started) {
        return H2_AUDIO_OK;
    }
    int ret = rtos_create_thread(&state->mic_thread,
        BEKEN_DEFAULT_WORKER_PRIORITY,
        "h2_audio_mic",
        (beken_thread_function_t)bk_audio_mic_task,
        4096,
        state);
    if (ret != kNoErr) {
        BK_LOGE(TAG, "audio mic task create failed=%d\r\n", ret);
        return H2_AUDIO_ERR_IO;
    }
    state->mic_thread_started = true;
    return H2_AUDIO_OK;
}

static int bk_audio_start_playback_task(h2_bk_audio_state_t *state) {
    if (state->playback_thread_started) {
        return H2_AUDIO_OK;
    }
    int ret = rtos_create_thread(&state->playback_thread,
        BEKEN_DEFAULT_WORKER_PRIORITY,
        "h2_audio_mix",
        (beken_thread_function_t)bk_audio_playback_task,
        4096,
        state);
    if (ret != kNoErr) {
        BK_LOGE(TAG, "audio mixer task create failed=%d\r\n", ret);
        return H2_AUDIO_ERR_IO;
    }
    state->playback_thread_started = true;
    return H2_AUDIO_OK;
}

static int bk_audio_ensure_speaker(h2_bk_audio_state_t *state) {
    if (state->speaker_initialized) {
        return H2_AUDIO_OK;
    }

    const uint32_t frame_size =
        (uint32_t)h2_bk7258_audio_config.frame_samples_per_channel *
        (uint32_t)h2_bk7258_audio_config.speaker_channels *
        ((uint32_t)h2_bk7258_audio_config.bits_per_sample / 8u);
    const uint32_t pool_size = frame_size * (uint32_t)h2_bk7258_audio_config.frame_count;

    audio_play_cfg_t cfg = DEFAULT_AUDIO_PLAY_CONFIG();
    cfg.nChans = h2_bk7258_audio_config.speaker_channels;
    cfg.sampRate = h2_bk7258_audio_config.sample_rate;
    cfg.bitsPerSample = h2_bk7258_audio_config.bits_per_sample;
    cfg.volume = bk_volume_from_percent(state->speaker_volume_percent);
    cfg.frame_size = frame_size;
    cfg.pool_size = pool_size;
    cfg.play_mode = AUDIO_PLAY_MODE_DIFFEN;

    state->play = audio_play_create(AUDIO_PLAY_ONBOARD_SPEAKER, &cfg);
    if (state->play == NULL) {
        BK_LOGE(TAG, "audio_play_create failed\r\n");
        return H2_AUDIO_ERR_IO;
    }

    state->speaker_initialized = true;
    return H2_AUDIO_OK;
}

static int bk_audio_ensure_mic(h2_bk_audio_state_t *state) {
    if (state->mic_initialized) {
        return H2_AUDIO_OK;
    }

    const uint32_t frame_size =
        (uint32_t)h2_bk7258_audio_config.frame_samples_per_channel *
        (uint32_t)h2_bk7258_audio_config.mic_channels *
        ((uint32_t)h2_bk7258_audio_config.bits_per_sample / 8u);
    const uint32_t pool_size = frame_size * (uint32_t)h2_bk7258_audio_config.frame_count;

    audio_record_cfg_t cfg = DEFAULT_AUDIO_RECORD_CONFIG();
    cfg.nChans = h2_bk7258_audio_config.mic_channels;
    cfg.sampRate = h2_bk7258_audio_config.sample_rate;
    cfg.bitsPerSample = h2_bk7258_audio_config.bits_per_sample;
    cfg.adc_gain = h2_bk7258_audio_config.default_mic_gain;
    cfg.mic_mode = AUDIO_MIC_MODE_DIFFEN;
    cfg.frame_size = frame_size;
    cfg.pool_size = pool_size;

    state->record = audio_record_create(AUDIO_RECORD_ONBOARD_MIC, &cfg);
    if (state->record == NULL) {
        BK_LOGE(TAG, "audio_record_create failed\r\n");
        return H2_AUDIO_ERR_IO;
    }

    state->mic_initialized = true;
    return H2_AUDIO_OK;
}

static int bk_audio_get_info(void *user, h2_audio_info_t *info) {
    (void)user;
    memset(info, 0, sizeof(*info));
    info->available = 1;
    info->mic_supported = h2_bk7258_audio_config.mic_channels > 0u;
    info->playback_supported = h2_bk7258_audio_config.speaker_channels > 0u;
    info->mic_format = bk_mic_format();
    info->playback_format = bk_playback_format();
    info->mic_queue_frames = H2_BK_AUDIO_MIC_QUEUE_FRAMES;
    info->track_queue_frames = H2_BK_AUDIO_TRACK_QUEUE_FRAMES;
    info->max_tracks = H2_BK_AUDIO_MAX_TRACKS;
    return H2_AUDIO_OK;
}

static int bk_audio_open(void *user) {
    h2_bk_audio_state_t *state = (h2_bk_audio_state_t *)user;
    state->opened = true;
    return H2_AUDIO_OK;
}

static int bk_audio_start_mic(void *user) {
    h2_bk_audio_state_t *state = (h2_bk_audio_state_t *)user;
    (void)bk_audio_open(user);
    int rc = bk_audio_ensure_mic(state);
    if (rc != H2_AUDIO_OK) {
        return rc;
    }
    rc = bk_audio_init_mic_queue(state);
    if (rc != H2_AUDIO_OK) {
        return rc;
    }
    rc = bk_audio_init_aec(state);
    if (rc != H2_AUDIO_OK) {
        return rc;
    }
    if (!state->mic_record_opened) {
        rc = map_bk_rc(audio_record_open(state->record));
        if (rc != H2_AUDIO_OK) {
            return rc;
        }
        state->mic_record_opened = true;
    } else {
        rc = map_bk_rc(audio_record_control(state->record, AUDIO_RECORD_RESUME));
        if (rc != H2_AUDIO_OK) {
            return rc;
        }
    }
    state->mic_open = true;
    (void)h2_pal_queue_reset(h2_bk7258_board_queue_api(), state->mic_queue);
    return bk_audio_start_mic_task(state);
}

static int bk_audio_read_mic(
    void *user,
    h2_audio_frame_t *out_frame,
    uint32_t timeout_ms) {
    h2_bk_audio_state_t *state = (h2_bk_audio_state_t *)user;
    if (!state->mic_open || state->mic_queue == NULL) {
        return H2_AUDIO_ERR_INVALID_STATE;
    }
    const h2_audio_pcm_format_t format = bk_mic_format();
    if (out_frame->sample_rate_hz != format.sample_rate_hz ||
        out_frame->channels != format.channels ||
        out_frame->sample_format != format.sample_format) {
        return H2_AUDIO_ERR_UNSUPPORTED;
    }
    const size_t frame_bytes = h2_audio_frame_frame_bytes(out_frame);
    if (frame_bytes == 0u || out_frame->capacity < frame_bytes) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }

    h2_bk_mic_queue_frame_t item;
    int rc = h2_pal_queue_recv(h2_bk7258_board_queue_api(), state->mic_queue, &item, timeout_ms);
    if (rc != H2_PAL_QUEUE_OK) {
        return map_queue_recv_rc(rc);
    }
    if (item.bytes > out_frame->capacity) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    os_memcpy(out_frame->data, item.samples, item.bytes);
    out_frame->bytes = item.bytes;
    out_frame->samples_per_channel = item.samples_per_channel;
    return H2_AUDIO_OK;
}

static int bk_audio_stop_mic(void *user) {
    h2_bk_audio_state_t *state = (h2_bk_audio_state_t *)user;
    if (state->record == NULL || !state->mic_record_opened) {
        return H2_AUDIO_OK;
    }
    state->mic_open = false;
    if (state->mic_queue != NULL) {
        (void)h2_pal_queue_reset(h2_bk7258_board_queue_api(), state->mic_queue);
    }
    int rc = map_bk_rc(audio_record_close(state->record));
    if (rc == H2_AUDIO_OK) {
        for (uint32_t i = 0u; state->mic_reading && i < 25u; ++i) {
            rtos_delay_milliseconds(2);
        }
        audio_record_destroy(state->record);
        state->record = NULL;
        state->mic_initialized = false;
        state->mic_record_opened = false;
    }
    return rc;
}

static int bk_audio_start_speaker(void *user) {
    h2_bk_audio_state_t *state = (h2_bk_audio_state_t *)user;
    int rc = bk_audio_open(user);
    if (rc != H2_AUDIO_OK) {
        return rc;
    }
    rc = bk_audio_ensure_speaker(state);
    if (rc != H2_AUDIO_OK) {
        return rc;
    }
    if (!state->speaker_started) {
        rc = map_bk_rc(audio_play_open(state->play));
        if (rc != H2_AUDIO_OK) {
            return rc;
        }
        state->speaker_started = true;
        state->speaker_open = true;
        rc = bk_audio_init_mixer(state);
        if (rc != H2_AUDIO_OK) {
            return rc;
        }
        return bk_audio_start_playback_task(state);
    }
    if (state->speaker_open) {
        return bk_audio_start_playback_task(state);
    }
    rc = map_bk_rc(audio_play_control(state->play, AUDIO_PLAY_RESUME));
    if (rc != H2_AUDIO_OK) {
        return rc;
    }
    rc = map_bk_rc(audio_play_control(state->play, AUDIO_PLAY_UNMUTE));
    if (rc != H2_AUDIO_OK) {
        return rc;
    }
    state->speaker_open = true;
    rc = bk_audio_init_mixer(state);
    if (rc != H2_AUDIO_OK) {
        return rc;
    }
    return bk_audio_start_playback_task(state);
}

static int bk_audio_stop_speaker(void *user) {
    h2_bk_audio_state_t *state = (h2_bk_audio_state_t *)user;
    if (state->play == NULL || !state->speaker_open) {
        return H2_AUDIO_OK;
    }
    int rc = map_bk_rc(audio_play_control(state->play, AUDIO_PLAY_MUTE));
    state->speaker_open = false;
    int pause_rc = map_bk_rc(audio_play_control(state->play, AUDIO_PLAY_PAUSE));
    return rc == H2_AUDIO_OK ? pause_rc : rc;
}

static int bk_audio_create_track(
    void *user,
    const h2_audio_track_config_t *config,
    h2_pal_audio_track_t **out_track) {
    h2_bk_audio_state_t *state = (h2_bk_audio_state_t *)user;
    if (config->format.sample_rate_hz != h2_bk7258_audio_config.sample_rate ||
        config->format.sample_format != H2_AUDIO_SAMPLE_S16LE ||
        config->format.channels != h2_bk7258_audio_config.speaker_channels) {
        return H2_AUDIO_ERR_UNSUPPORTED;
    }
    int rc = bk_audio_init_mixer(state);
    if (rc != H2_AUDIO_OK) {
        return rc;
    }
    return h2_audio_mixer_create_track(&state->mixer, NULL, config, out_track);
}

static int bk_audio_get_speaker_volume_percent(void *user, uint32_t *out_percent) {
    h2_bk_audio_state_t *state = (h2_bk_audio_state_t *)user;
    *out_percent = state->speaker_volume_percent;
    return H2_AUDIO_OK;
}

static int bk_audio_set_speaker_volume_percent(void *user, uint32_t percent) {
    h2_bk_audio_state_t *state = (h2_bk_audio_state_t *)user;
    state->speaker_volume_percent = percent;
    if (state->play == NULL) {
        return H2_AUDIO_OK;
    }
    return map_bk_rc(audio_play_set_volume(state->play, bk_volume_from_percent(percent)));
}

h2_pal_audio_t *h2_bk7258_board_audio(void) {
    static const h2_pal_audio_vtable_t vtable = {
        .get_info = bk_audio_get_info,
        .start_mic = bk_audio_start_mic,
        .stop_mic = bk_audio_stop_mic,
        .start_speaker = bk_audio_start_speaker,
        .stop_speaker = bk_audio_stop_speaker,
        .mic_read = bk_audio_read_mic,
        .create_track = bk_audio_create_track,
        .get_speaker_volume_percent = bk_audio_get_speaker_volume_percent,
        .set_speaker_volume_percent = bk_audio_set_speaker_volume_percent,
    };
    static h2_pal_audio_t audio = {
        .user = &s_audio_state,
        .vtable = &vtable,
    };
    return &audio;
}
