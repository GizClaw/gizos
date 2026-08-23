#include "h2_smoke_audio_system.h"

#include "opus.h"

#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define H2_SMOKE_AUDIO_DEFAULT_MUSIC_PATH "/data/audio/music_loop.ogg"
#define H2_SMOKE_AUDIO_OPUS_DECODER_STORAGE_SIZE 19000u
#define H2_SMOKE_AUDIO_OGG_PACKET_MAX 4096u
#define H2_SMOKE_AUDIO_OPUS_MAX_FRAME_SAMPLES 5760u
#define H2_SMOKE_AUDIO_FRAME_SAMPLE_CAP (H2_SMOKE_AUDIO_OPUS_MAX_FRAME_SAMPLES * 2u)
#define H2_SMOKE_AUDIO_MUSIC_PCM_CAP 16384u
#define H2_SMOKE_AUDIO_IO_TIMEOUT_MS 1000u
#define H2_SMOKE_AUDIO_ERROR_SLEEP_MS 1000u
#define H2_SMOKE_AUDIO_MUSIC_TRACK_VOLUME 350u
#define H2_SMOKE_AUDIO_MIC_TRACK_VOLUME 1000u

typedef struct ogg_opus_cursor {
    const uint8_t *data;
    size_t size;
    size_t page_offset;
    const uint8_t *segments;
    uint8_t segment_count;
    uint8_t segment_index;
    const uint8_t *payload;
    size_t payload_left;
    uint8_t packet[H2_SMOKE_AUDIO_OGG_PACKET_MAX];
    size_t packet_len;
} ogg_opus_cursor_t;

typedef struct audio_scene {
    const h2_pal_audio_api_t *audio;
    const h2_pal_task_api_t *task;
    const h2_pal_time_api_t *time;
    const h2_pal_mem_api_t *allocator;
    h2_audio_info_t info;
    h2_pal_audio_track_t *music_track;
    h2_pal_audio_track_t *mic_track;
    h2_pal_task_t *music_task;
    h2_pal_task_t *mic_task;
    atomic_bool stop_requested;
    int active;
    uint8_t *music_data;
    size_t music_len;
    ogg_opus_cursor_t cursor;
    long decoder_storage[(H2_SMOKE_AUDIO_OPUS_DECODER_STORAGE_SIZE + sizeof(long) - 1u) / sizeof(long)];
    int16_t decoded[H2_SMOKE_AUDIO_OPUS_MAX_FRAME_SAMPLES * 2u];
    int16_t music_pcm[H2_SMOKE_AUDIO_MUSIC_PCM_CAP];
    size_t music_pcm_pos;
    size_t music_pcm_count;
    int16_t music_frame[H2_SMOKE_AUDIO_FRAME_SAMPLE_CAP];
    int16_t mic_frame[H2_SMOKE_AUDIO_FRAME_SAMPLE_CAP];
    uint32_t mic_frame_count;
} audio_scene_t;

static audio_scene_t s_scene;

static size_t sample_count_for_format(const h2_audio_pcm_format_t *format) {
    if (format == NULL) {
        return 0u;
    }
    return (size_t)format->frame_samples_per_channel * (size_t)format->channels;
}

static int validate_config(
    h2_runtime_t *runtime,
    const h2_smoke_audio_system_config_t *config) {
    if (runtime == NULL || config == NULL ||
        runtime->audio == NULL || runtime->fs == NULL ||
        runtime->mem == NULL || runtime->task == NULL ||
        runtime->time == NULL || config->speaker_volume_percent > 100u) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    return H2_AUDIO_OK;
}

static int load_file(
    const h2_pal_fs_api_t *fs,
    const h2_pal_mem_api_t *allocator,
    const char *path,
    uint8_t **out_data,
    size_t *out_len) {
    if (fs == NULL || allocator == NULL ||
        path == NULL || out_data == NULL || out_len == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    *out_data = NULL;
    *out_len = 0u;

    h2_pal_fs_stat_t stat;
    int rc = h2_pal_fs_stat(fs, path, &stat);
    if (rc != H2_PAL_FS_OK) {
        return rc;
    }
    if (stat.is_dir || stat.size == 0u || stat.size > SIZE_MAX) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }

    uint8_t *data = (uint8_t *)h2_pal_mem_alloc(allocator, (size_t)stat.size);
    if (data == NULL) {
        return H2_PAL_FS_ERR_NO_MEMORY;
    }

    h2_pal_fs_file_t *file = NULL;
    rc = h2_pal_fs_open(fs, path, H2_PAL_FS_OPEN_READ, &file);
    if (rc != H2_PAL_FS_OK) {
        h2_pal_mem_free(allocator, data);
        return rc;
    }

    size_t offset = 0u;
    while (offset < (size_t)stat.size) {
        size_t nread = 0u;
        rc = h2_pal_fs_read(fs, file, data + offset, (size_t)stat.size - offset, &nread);
        if (rc != H2_PAL_FS_OK) {
            break;
        }
        if (nread == 0u) {
            rc = H2_PAL_ERR_TRUNCATED;
            break;
        }
        offset += nread;
    }

    int close_rc = h2_pal_fs_close(fs, file);
    if (rc != H2_PAL_FS_OK || close_rc != H2_PAL_FS_OK) {
        h2_pal_mem_free(allocator, data);
        return rc != H2_PAL_FS_OK ? rc : close_rc;
    }

    *out_data = data;
    *out_len = offset;
    return H2_PAL_FS_OK;
}

static int validate_audio_info(const h2_audio_info_t *info) {
    if (info == NULL || !info->available || !info->mic_supported ||
        !info->playback_supported || info->max_tracks < 2u) {
        return H2_AUDIO_ERR_UNSUPPORTED;
    }
    if (info->mic_format.sample_format != H2_AUDIO_SAMPLE_S16LE ||
        info->playback_format.sample_format != H2_AUDIO_SAMPLE_S16LE ||
        info->mic_format.channels != info->playback_format.channels ||
        info->mic_format.sample_rate_hz != info->playback_format.sample_rate_hz) {
        return H2_AUDIO_ERR_UNSUPPORTED;
    }
    if (sample_count_for_format(&info->playback_format) > H2_SMOKE_AUDIO_FRAME_SAMPLE_CAP ||
        sample_count_for_format(&info->mic_format) > H2_SMOKE_AUDIO_FRAME_SAMPLE_CAP) {
        return H2_AUDIO_ERR_NO_MEMORY;
    }
    return H2_AUDIO_OK;
}

static void ogg_cursor_reset(ogg_opus_cursor_t *cursor, const uint8_t *data, size_t size) {
    memset(cursor, 0, sizeof(*cursor));
    cursor->data = data;
    cursor->size = size;
}

static int ogg_cursor_load_page(ogg_opus_cursor_t *cursor) {
    if (cursor->page_offset + 27u > cursor->size) {
        return 0;
    }

    const uint8_t *page = cursor->data + cursor->page_offset;
    if (memcmp(page, "OggS", 4u) != 0) {
        return -1;
    }

    uint8_t segment_count = page[26u];
    size_t header_len = 27u + (size_t)segment_count;
    if (cursor->page_offset + header_len > cursor->size) {
        return -1;
    }

    const uint8_t *segments = page + 27u;
    size_t payload_len = 0u;
    for (uint8_t i = 0u; i < segment_count; ++i) {
        payload_len += segments[i];
    }
    if (cursor->page_offset + header_len + payload_len > cursor->size) {
        return -1;
    }

    cursor->segments = segments;
    cursor->segment_count = segment_count;
    cursor->segment_index = 0u;
    cursor->payload = page + header_len;
    cursor->payload_left = payload_len;
    cursor->page_offset += header_len + payload_len;
    return 1;
}

static int ogg_cursor_next_packet(ogg_opus_cursor_t *cursor, const uint8_t **out_packet, size_t *out_len) {
    cursor->packet_len = 0u;

    for (;;) {
        if (cursor->segment_index >= cursor->segment_count) {
            int page_rc = ogg_cursor_load_page(cursor);
            if (page_rc <= 0) {
                return page_rc;
            }
        }

        while (cursor->segment_index < cursor->segment_count) {
            uint8_t segment_len = cursor->segments[cursor->segment_index++];
            if (cursor->payload_left < segment_len ||
                cursor->packet_len + (size_t)segment_len > sizeof(cursor->packet)) {
                return -1;
            }
            if (segment_len > 0u) {
                memcpy(cursor->packet + cursor->packet_len, cursor->payload, segment_len);
                cursor->payload += segment_len;
                cursor->payload_left -= segment_len;
                cursor->packet_len += segment_len;
            }
            if (segment_len < 255u) {
                *out_packet = cursor->packet;
                *out_len = cursor->packet_len;
                return 1;
            }
        }
    }
}

static int decoder_init(audio_scene_t *scene) {
    int decoder_size = opus_decoder_get_size(scene->info.playback_format.channels);
    if (decoder_size <= 0 || (size_t)decoder_size > sizeof(scene->decoder_storage)) {
        return H2_AUDIO_ERR_NO_MEMORY;
    }

    OpusDecoder *decoder = (OpusDecoder *)(void *)scene->decoder_storage;
    int err = opus_decoder_init(
        decoder,
        (opus_int32)scene->info.playback_format.sample_rate_hz,
        scene->info.playback_format.channels);
    scene->music_pcm_pos = 0u;
    scene->music_pcm_count = 0u;
    return err == OPUS_OK ? H2_AUDIO_OK : H2_AUDIO_ERR_IO;
}

static int decode_more(audio_scene_t *scene) {
    if (scene->music_pcm_pos > 0u && scene->music_pcm_pos < scene->music_pcm_count) {
        size_t remaining = scene->music_pcm_count - scene->music_pcm_pos;
        memmove(scene->music_pcm, scene->music_pcm + scene->music_pcm_pos, remaining * sizeof(scene->music_pcm[0]));
        scene->music_pcm_pos = 0u;
        scene->music_pcm_count = remaining;
    } else if (scene->music_pcm_pos >= scene->music_pcm_count) {
        scene->music_pcm_pos = 0u;
        scene->music_pcm_count = 0u;
    }

    const uint8_t *packet = NULL;
    size_t packet_len = 0u;
    for (;;) {
        int packet_rc = ogg_cursor_next_packet(&scene->cursor, &packet, &packet_len);
        if (packet_rc == 0) {
            ogg_cursor_reset(&scene->cursor, scene->music_data, scene->music_len);
            int rc = decoder_init(scene);
            if (rc != H2_AUDIO_OK) {
                return rc;
            }
            continue;
        }
        if (packet_rc < 0) {
            return H2_AUDIO_ERR_IO;
        }
        if (packet_len >= 8u &&
            (memcmp(packet, "OpusHead", 8u) == 0 || memcmp(packet, "OpusTags", 8u) == 0)) {
            continue;
        }
        break;
    }

    OpusDecoder *decoder = (OpusDecoder *)(void *)scene->decoder_storage;
    int decoded_samples = opus_decode(
        decoder,
        packet,
        (opus_int32)packet_len,
        scene->decoded,
        H2_SMOKE_AUDIO_OPUS_MAX_FRAME_SAMPLES,
        0);
    if (decoded_samples <= 0) {
        return H2_AUDIO_ERR_IO;
    }

    size_t decoded_count = (size_t)decoded_samples * (size_t)scene->info.playback_format.channels;
    if (scene->music_pcm_count + decoded_count > H2_SMOKE_AUDIO_MUSIC_PCM_CAP) {
        return H2_AUDIO_ERR_NO_MEMORY;
    }
    memcpy(scene->music_pcm + scene->music_pcm_count, scene->decoded, decoded_count * sizeof(scene->music_pcm[0]));
    scene->music_pcm_count += decoded_count;
    return H2_AUDIO_OK;
}

static int music_write_once(audio_scene_t *scene) {
    size_t frame_samples = sample_count_for_format(&scene->info.playback_format);
    if (frame_samples == 0u || frame_samples > sizeof(scene->music_frame) / sizeof(scene->music_frame[0])) {
        return H2_AUDIO_ERR_NO_MEMORY;
    }

    while (scene->music_pcm_count - scene->music_pcm_pos < frame_samples) {
        int rc = decode_more(scene);
        if (rc != H2_AUDIO_OK) {
            return rc;
        }
    }

    memcpy(scene->music_frame, scene->music_pcm + scene->music_pcm_pos, frame_samples * sizeof(scene->music_frame[0]));
    scene->music_pcm_pos += frame_samples;

    h2_audio_frame_t frame = h2_audio_frame_for_buffer(
        scene->music_frame,
        frame_samples * sizeof(scene->music_frame[0]),
        scene->info.playback_format);
    frame.bytes = frame_samples * sizeof(scene->music_frame[0]);
    frame.samples_per_channel = scene->info.playback_format.frame_samples_per_channel;
    return h2_pal_audio_track_write(
        scene->music_track,
        &frame,
        H2_SMOKE_AUDIO_IO_TIMEOUT_MS);
}

static int mic_loop_once(audio_scene_t *scene) {
    h2_audio_frame_t mic = h2_audio_frame_for_buffer(scene->mic_frame, sizeof(scene->mic_frame), scene->info.mic_format);
    int rc = h2_pal_audio_mic_read(scene->audio, &mic, H2_SMOKE_AUDIO_IO_TIMEOUT_MS);
    if (rc == H2_AUDIO_ERR_WOULD_BLOCK) {
        return H2_AUDIO_OK;
    }
    if (rc != H2_AUDIO_OK || mic.bytes == 0u) {
        return rc;
    }
    if (mic.bytes > sizeof(scene->mic_frame)) {
        return H2_AUDIO_ERR_NO_MEMORY;
    }
    if (++scene->mic_frame_count % 64u == 0u) {
        int32_t peak = 0;
        size_t samples = mic.bytes / sizeof(scene->mic_frame[0]);
        for (size_t i = 0u; i < samples; ++i) {
            int32_t value = scene->mic_frame[i];
            int32_t magnitude = value < 0 ? -value : value;
            if (magnitude > peak) {
                peak = magnitude;
            }
        }
        printf("H2_SMOKE_AUDIO_MIC peak=%ld\n", (long)peak);
    }

    h2_audio_frame_t playback = h2_audio_frame_for_buffer(scene->mic_frame, mic.bytes, scene->info.playback_format);
    playback.bytes = mic.bytes;
    playback.samples_per_channel = mic.samples_per_channel;
    return h2_pal_audio_track_write(scene->mic_track, &playback, 100u);
}

static void sleep_after_error(audio_scene_t *scene) {
    if (scene->time != NULL) {
        (void)h2_pal_time_sleep_ms(scene->time, H2_SMOKE_AUDIO_ERROR_SLEEP_MS);
    }
}

static void music_task(void *ctx) {
    audio_scene_t *scene = (audio_scene_t *)ctx;
    while (!atomic_load_explicit(&scene->stop_requested, memory_order_acquire)) {
        if (music_write_once(scene) != H2_AUDIO_OK &&
            !atomic_load_explicit(&scene->stop_requested, memory_order_acquire)) {
            sleep_after_error(scene);
        }
    }
}

static void mic_task(void *ctx) {
    audio_scene_t *scene = (audio_scene_t *)ctx;
    while (!atomic_load_explicit(&scene->stop_requested, memory_order_acquire)) {
        if (mic_loop_once(scene) != H2_AUDIO_OK &&
            !atomic_load_explicit(&scene->stop_requested, memory_order_acquire)) {
            sleep_after_error(scene);
        }
    }
}

static int join_task(h2_pal_task_t **task) {
    if (*task == NULL) {
        return H2_PAL_OK;
    }
    int rc = h2_pal_task_join(s_scene.task, *task);
    if (rc == H2_PAL_OK) {
        *task = NULL;
    }
    return rc;
}

static int close_track(h2_pal_audio_track_t **track) {
    if (*track == NULL) {
        return H2_AUDIO_OK;
    }
    int rc = h2_pal_audio_track_close(*track);
    if (rc == H2_AUDIO_OK) {
        *track = NULL;
    }
    return rc;
}

int h2_smoke_audio_system_stop(void) {
    if (!s_scene.active) {
        return H2_AUDIO_OK;
    }

    atomic_store_explicit(&s_scene.stop_requested, true, memory_order_release);
    if (s_scene.audio != NULL) {
        (void)h2_pal_audio_stop_mic(s_scene.audio);
        (void)h2_pal_audio_stop_speaker(s_scene.audio);
    }

    int rc = join_task(&s_scene.music_task);
    int task_rc = join_task(&s_scene.mic_task);
    if (rc == H2_PAL_OK) {
        rc = task_rc;
    }
    if (rc != H2_PAL_OK) {
        return rc;
    }

    rc = close_track(&s_scene.music_track);
    int track_rc = close_track(&s_scene.mic_track);
    if (rc == H2_AUDIO_OK) {
        rc = track_rc;
    }
    if (rc != H2_AUDIO_OK) {
        return rc;
    }

    if (s_scene.music_data != NULL && s_scene.allocator != NULL) {
        h2_pal_mem_free(s_scene.allocator, s_scene.music_data);
    }
    memset(&s_scene, 0, sizeof(s_scene));
    return H2_AUDIO_OK;
}

static int fail_after_cleanup(int error_code) {
    int cleanup_rc = h2_smoke_audio_system_stop();
    return cleanup_rc == H2_AUDIO_OK ? error_code : cleanup_rc;
}

int h2_smoke_audio_system_run(
    h2_runtime_t *runtime,
    const h2_smoke_audio_system_config_t *config) {
    int rc = validate_config(runtime, config);
    if (rc != H2_AUDIO_OK) {
        return rc;
    }
    if (s_scene.active) {
        return H2_AUDIO_ERR_INVALID_STATE;
    }

    memset(&s_scene, 0, sizeof(s_scene));
    atomic_init(&s_scene.stop_requested, false);
    s_scene.active = 1;
    s_scene.audio = runtime->audio;
    s_scene.task = runtime->task;
    s_scene.time = runtime->time;
    s_scene.allocator = runtime->mem;

    const char *music_path = config->music_path != NULL ?
        config->music_path :
        H2_SMOKE_AUDIO_DEFAULT_MUSIC_PATH;

    rc = load_file(
        runtime->fs,
        runtime->mem,
        music_path,
        &s_scene.music_data,
        &s_scene.music_len);
    if (rc != H2_PAL_FS_OK) {
        return fail_after_cleanup(rc);
    }
    if (s_scene.music_data == NULL || s_scene.music_len == 0u) {
        return fail_after_cleanup(H2_PAL_FS_ERR_INVALID_ARG);
    }

    rc = h2_pal_audio_get_info(s_scene.audio, &s_scene.info);
    if (rc == H2_AUDIO_OK) {
        rc = validate_audio_info(&s_scene.info);
    }
    if (rc != H2_AUDIO_OK) {
        return fail_after_cleanup(rc);
    }

    ogg_cursor_reset(&s_scene.cursor, s_scene.music_data, s_scene.music_len);
    rc = decoder_init(&s_scene);
    if (rc != H2_AUDIO_OK) {
        return fail_after_cleanup(rc);
    }

    rc = h2_pal_audio_start_mic(s_scene.audio);
    if (rc == H2_AUDIO_OK) {
        const uint32_t speaker_volume_percent = config->speaker_volume_percent == 0u ?
            100u : config->speaker_volume_percent;
        (void)h2_pal_audio_set_speaker_volume_percent(s_scene.audio, speaker_volume_percent);
        rc = h2_pal_audio_start_speaker(s_scene.audio);
    }

    h2_audio_track_config_t music_config = {
        .name = "audio-system-music",
        .format = s_scene.info.playback_format,
        .volume_factor_milli = H2_SMOKE_AUDIO_MUSIC_TRACK_VOLUME,
        .buffer_frames = 8u,
    };
    if (rc == H2_AUDIO_OK) {
        rc = h2_pal_audio_create_track(s_scene.audio, &music_config, &s_scene.music_track);
    }

    h2_audio_track_config_t mic_config = {
        .name = "audio-system-mic",
        .format = s_scene.info.playback_format,
        .volume_factor_milli = H2_SMOKE_AUDIO_MIC_TRACK_VOLUME,
        .buffer_frames = 4u,
    };
    if (rc == H2_AUDIO_OK) {
        rc = h2_pal_audio_create_track(s_scene.audio, &mic_config, &s_scene.mic_track);
    }
    if (rc == H2_AUDIO_OK) {
        rc = music_write_once(&s_scene);
    }
    if (rc != H2_AUDIO_OK) {
        return fail_after_cleanup(rc);
    }

    const h2_pal_task_options_t music_task_options = {
        .name = "audio-system-music",
        .min_stack_size = 24576u,
    };
    const h2_pal_task_options_t mic_task_options = {
        .name = "audio-system-mic",
        .min_stack_size = 8192u,
    };
    if (h2_pal_task_start(s_scene.task, &music_task_options, music_task, &s_scene, &s_scene.music_task) != H2_PAL_OK ||
        h2_pal_task_start(s_scene.task, &mic_task_options, mic_task, &s_scene, &s_scene.mic_task) != H2_PAL_OK) {
        return fail_after_cleanup(H2_AUDIO_ERR_NO_MEMORY);
    }

    return H2_AUDIO_OK;
}
