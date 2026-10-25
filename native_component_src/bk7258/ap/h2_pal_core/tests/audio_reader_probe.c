#include "h2_bk_audio_reader.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum { H2_AUDIO_OK = 0, H2_AUDIO_ERR_IO = -4, AUDIO_RECORD_PAUSE = 0 };
typedef struct {
  bool alive, paused, buffers_owned, closed;
} record_t;
typedef struct {
  record_t* record;
  void* mic_queue;
  bool mic_initialized, mic_record_opened, mic_retiring;
  atomic_bool mic_open, mic_reading;
} h2_bk_audio_state_t;
static h2_bk_audio_state_t* active;
static unsigned waited, release_after, closes, destroys, pauses;
static int pause_error, close_error, destroy_error;
static void* h2_bk7258_board_queue_api(void) { return NULL; }
static int h2_pal_queue_reset(void* api, void* queue) {
  (void)api;
  (void)queue;
  return 0;
}
static int map_bk_rc(int rc) { return rc == 0 ? H2_AUDIO_OK : H2_AUDIO_ERR_IO; }
static int audio_record_control(record_t* record, int control) {
  assert(control == AUDIO_RECORD_PAUSE && record->alive &&
         record->buffers_owned);
  ++pauses;
  if (pause_error) return -1;
  record->paused =
      true; /* SDK pause wakes the read, keeping all buffers live. */
  return 0;
}
static int audio_record_close(record_t* record) {
  assert(record->alive && record->paused);
  assert(h2_bk_audio_reader_idle(&active->mic_reading));
  ++closes;
  if (close_error) return -1;
  record->closed = true;
  record->buffers_owned = false;
  return 0;
}
static int audio_record_destroy(record_t* record) {
  assert(record->alive);
  assert(h2_bk_audio_reader_idle(&active->mic_reading));
  ++destroys;
  if (destroy_error) return -1;
  const int closed = audio_record_close(record);
  if (closed != 0) return closed;
  record->alive = false;
  return 0;
}
static void rtos_delay_milliseconds(unsigned ms) {
  waited += ms;
  assert(active->record->alive && active->record->buffers_owned);
  if (waited >= release_after) h2_bk_audio_reader_leave(&active->mic_reading);
}
#define TAG "test"
#define BK_LOGE(...) ((void)0)
#include "h2_audio_reader_stop.inc"

static void reset(h2_bk_audio_state_t* state, record_t* record) {
  *record = (record_t){.alive = true, .buffers_owned = true};
  state->record = record;
  state->mic_queue = NULL;
  state->mic_initialized = state->mic_record_opened = true;
  state->mic_retiring = false;
  atomic_init(&state->mic_open, true);
  atomic_init(&state->mic_reading, false);
  active = state;
  waited = closes = destroys = pauses = 0;
  release_after = UINT32_MAX;
  pause_error = close_error = destroy_error = 0;
}
int main(void) {
  h2_bk_audio_state_t state;
  record_t record;
  reset(&state, &record);
  assert(h2_bk_audio_reader_enter(&state.mic_open, &state.mic_reading));
  release_after = 40;
  assert(bk_audio_stop_mic(&state) == H2_AUDIO_OK);
  assert(waited == 40 && closes == 1 && destroys == 1 && !record.alive);
  assert(state.record == NULL && !state.mic_record_opened);
  assert(bk_audio_stop_mic(&state) == H2_AUDIO_OK && destroys == 1);
  assert(!h2_bk_audio_reader_enter(&state.mic_open, &state.mic_reading));
  assert(h2_bk_audio_reader_idle(&state.mic_reading));

  reset(&state, &record);
  assert(h2_bk_audio_reader_enter(&state.mic_open, &state.mic_reading));
  assert(bk_audio_stop_mic(&state) == H2_AUDIO_ERR_IO);
  assert(waited == 50 && closes == 0 && destroys == 0);
  assert(state.record == &record && record.alive && record.buffers_owned);
  assert(!state.mic_record_opened && state.mic_retiring &&
         state.mic_initialized && !state.mic_open);
  /* A descheduled reader can safely finish after the original wait expired. */
  assert(record.paused && record.alive && record.buffers_owned);
  h2_bk_audio_reader_leave(&state.mic_reading);
  assert(bk_audio_stop_mic(&state) == H2_AUDIO_OK && destroys == 1);

  for (unsigned failure = 0; failure < 3; ++failure) {
    reset(&state, &record);
    if (failure == 0) pause_error = 1;
    if (failure == 1) close_error = 1;
    if (failure == 2) destroy_error = 1;
    assert(bk_audio_stop_mic(&state) == H2_AUDIO_ERR_IO);
    assert(state.record == &record && record.alive && state.mic_retiring);
    pause_error = close_error = destroy_error = 0;
    assert(bk_audio_stop_mic(&state) == H2_AUDIO_OK && !record.alive);
  }
  puts(
      "PASS delayed reader, retained timeout/retry, admission and failed cleanup");
  return 0;
}
