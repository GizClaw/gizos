#ifndef H2_BLOOMSPEAKER_AUDIO_H
#define H2_BLOOMSPEAKER_AUDIO_H

#include "h2_bloomspeaker_controller.h"
#include "h2_runtime.h"

#include "h2_bleikcp.h"

#include <stdbool.h>

typedef struct h2_bloomspeaker_audio h2_bloomspeaker_audio_t;
typedef bool (*h2_bloomspeaker_audio_session_active_fn)(void *user);

int h2_bloomspeaker_audio_start(h2_runtime_t *runtime,
                                h2_bloomspeaker_controller_t *controller,
                                h2_bloomspeaker_audio_t **out_audio);

int h2_bloomspeaker_audio_run_session(
    h2_bloomspeaker_audio_t *audio, h2_bleikcp_t *stream,
    h2_bloomspeaker_audio_session_active_fn active, void *active_user);

int h2_bloomspeaker_audio_stop(h2_bloomspeaker_audio_t *audio);

#endif
