#ifndef H2_GAME_AUDIO_H
#define H2_GAME_AUDIO_H

#include "h2/pal/hal/h2_pal_audio.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/os/h2_pal_queue.h"
#include "h2/pal/os/h2_pal_task.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_GAME_AUDIO_OK 0
#define H2_GAME_AUDIO_ERR_INVALID_ARG -1
#define H2_GAME_AUDIO_ERR_NO_MEMORY -2
#define H2_GAME_AUDIO_ERR_AUDIO -3
#define H2_GAME_AUDIO_ERR_QUEUE -4
#define H2_GAME_AUDIO_ERR_TASK -5
#define H2_GAME_AUDIO_ERR_OVERFLOW -6

#define H2_GAME_AUDIO_SAMPLE_RATE_HZ 16000u

typedef struct h2_game_audio h2_game_audio_t;

typedef enum h2_game_audio_wave {
    H2_GAME_AUDIO_WAVE_PULSE = 0,
    H2_GAME_AUDIO_WAVE_TRIANGLE = 1,
    H2_GAME_AUDIO_WAVE_NOISE = 2,
    H2_GAME_AUDIO_WAVE_SINE = 3,
    H2_GAME_AUDIO_WAVE_SAW = 4,
} h2_game_audio_wave_t;

/** One sample-clock scheduled synthesized note. All values are integer and portable. */
typedef struct h2_game_audio_step {
    uint32_t start_ms;
    uint32_t duration_ms;
    uint16_t frequency_hz;
    uint16_t end_frequency_hz;
    uint16_t volume_permille;
    uint16_t duty_permille;
    h2_game_audio_wave_t wave;
} h2_game_audio_step_t;

typedef struct h2_game_audio_recipe {
    const h2_game_audio_step_t *steps;
    size_t step_count;
    uint32_t duration_ms;
} h2_game_audio_recipe_t;

typedef struct h2_game_audio_config {
    const h2_pal_audio_api_t *audio;
    const h2_pal_task_api_t *task;
    const h2_pal_queue_api_t *queue;
    const h2_pal_mem_api_t *mem;
    size_t command_capacity;
} h2_game_audio_config_t;

/**
 * Creates the PAL-backed PCM worker on an already-started speaker.
 * The PAL playback format must be 16 kHz mono S16LE; its frame size determines each generated write.
 * Config APIs remain borrowed.
 */
int h2_game_audio_create(const h2_game_audio_config_t *config, h2_game_audio_t **out_audio);
/** Queues a borrowed recipe; steps must be ordered by nondecreasing start_ms, fit within duration, and outlive playback. */
int h2_game_audio_play(h2_game_audio_t *audio, const h2_game_audio_recipe_t *recipe);
/** Publishes a borrowed priority recipe with the same ordering and duration rules without depending on queue capacity. */
int h2_game_audio_play_latest(h2_game_audio_t *audio, const h2_game_audio_recipe_t *recipe);
/** Stops and joins the PCM worker. */
int h2_game_audio_stop(h2_game_audio_t *audio);
/** Stops and destroys the worker. On join failure, ownership remains with the caller for retry. */
int h2_game_audio_destroy(h2_game_audio_t *audio);

#ifdef __cplusplus
}
#endif

#endif
