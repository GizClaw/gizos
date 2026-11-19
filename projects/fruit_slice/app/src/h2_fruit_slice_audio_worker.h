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
/* Close commands, join, return mixer ownership. On failure retain all handles
 * and context for retry; never access the worker's mixer before join succeeds.
 */
int h2_fruit_slice_audio_worker_stop(h2_fruit_slice_audio_t* audio);

#endif
