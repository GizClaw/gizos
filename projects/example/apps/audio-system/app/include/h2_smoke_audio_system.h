#ifndef H2_SMOKE_AUDIO_SYSTEM_H
#define H2_SMOKE_AUDIO_SYSTEM_H

#include "h2_runtime.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_smoke_audio_system_config {
    /** Optional music path; NULL uses the packaged smoke asset. */
    const char *music_path;
    /** Speaker volume in percent; zero preserves the historical 100% default. */
    uint32_t speaker_volume_percent;
} h2_smoke_audio_system_config_t;

/**
 * @brief Start the audio scene and its worker tasks.
 *
 * On success, the caller owns the running scene and must call
 * h2_smoke_audio_system_stop() before deinitializing Runtime. Startup failures
 * roll back all resources acquired by this call.
 */
int h2_smoke_audio_system_run(
    h2_runtime_t *runtime,
    const h2_smoke_audio_system_config_t *config);

/**
 * @brief Stop the audio workers and release resources retained by the app.
 *
 * This function blocks until every started worker task has joined. It must
 * complete successfully before the Runtime passed to
 * h2_smoke_audio_system_run() is deinitialized. Calling it while the app is
 * inactive succeeds without doing anything. Calls must be serialized with
 * h2_smoke_audio_system_run() and other stop calls.
 *
 * @return H2_AUDIO_OK on success, otherwise an audio or PAL task error. A
 * failed cleanup can be retried while the Runtime remains alive.
 */
int h2_smoke_audio_system_stop(void);

#ifdef __cplusplus
}
#endif

#endif
