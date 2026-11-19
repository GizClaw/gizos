#ifndef H2_FRUIT_SLICE_AUDIO_WORKER_H
#define H2_FRUIT_SLICE_AUDIO_WORKER_H

#include "h2_fruit_slice_audio.h"

/* Main-thread controller. Worker exclusively owns the copied mixer while
 * running; queues carry values, never game state or borrowed frame buffers. */
int h2_fruit_slice_audio_worker_start(h2_fruit_slice_audio_t* audio);
void h2_fruit_slice_audio_worker_emit(h2_fruit_slice_audio_t* audio,
                                      h2_fruit_slice_sound_t sound,
                                      uint64_t now_ms);
int h2_fruit_slice_audio_worker_poll(h2_fruit_slice_audio_t* audio);
/* Stop accepting commands immediately, confirm queue close/wake, then join
 * and return mixer ownership. Failed close is retried even if it changed the
 * queue predicate. Pending stop makes emit drop/count commands and poll return
 * OK; only a successful join permits mixer access or resource reclamation. */
int h2_fruit_slice_audio_worker_stop(h2_fruit_slice_audio_t* audio);

#endif
