/* Compile the complete production provider, replacing only SDK dependencies. */
#include "../ap/src/h2_bk7258_board_audio.c"

h2_bk7258_audio_config_t h2_bk7258_audio_config;
static int power_error, pause_error, destroy_error;
static int power_on, speaker_control_error = -1;
static unsigned power_calls[2], controls[AUDIO_PLAY_PAUSE + 1];
static audio_play_t test_play;
static int osi_error, malloc_error, mutex_error, missing_buffer, record_error;
static int open_error, queue_error, task_error, gain_error;
static unsigned frame_samples, allocations, frees, mutex_calls, record_creates,
    record_destroys, queue_creates;
static void* live_context;
static int16_t buffers[3][320];
static audio_record_t test_record;
static int queue_token, task_token;
void* test_malloc(size_t size) {
  if (malloc_error) return NULL;
  assert(live_context == NULL);
  live_context = malloc(size);
  assert(live_context != NULL);
  ++allocations;
  return live_context;
}
void test_free(void* ptr) {
  assert(ptr != NULL && ptr == live_context);
  free(ptr);
  live_context = NULL;
  ++frees;
}
uint32_t aec_size(uint32_t size) {
  (void)size;
  return sizeof(AECContext);
}
void aec_init(AECContext* ctx, int16_t rate) {
  assert(ctx == live_context);
  assert(rate == 8000 || rate == 16000);
}
void aec_ctrl(AECContext* ctx, int cmd, uintptr_t value) {
  assert(ctx == live_context);
  if (cmd == AEC_CTRL_CMD_GET_FRAME_SAMPLE)
    *(uint32_t*)value = frame_samples;
  else if (cmd >= AEC_CTRL_CMD_GET_RX_BUF && cmd <= AEC_CTRL_CMD_GET_OUT_BUF)
    *(uintptr_t*)value =
        cmd == missing_buffer
            ? 0
            : (uintptr_t)buffers[cmd - AEC_CTRL_CMD_GET_RX_BUF];
}
void aec_proc(AECContext* ctx, int16_t* ref, int16_t* mic, int16_t* out) {
  assert(ctx == live_context);
  (void)ref;
  memcpy(out, mic, sizeof(buffers[0]));
}
int bk_audio_osi_funcs_init(void) { return osi_error; }
int rtos_init_mutex(beken_mutex_t* mutex) {
  ++mutex_calls;
  if (mutex_error) return mutex_error;
  *mutex = 1;
  return 0;
}
int rtos_lock_mutex(beken_mutex_t* mutex) {
  assert(*mutex == 1);
  return 0;
}
int rtos_unlock_mutex(beken_mutex_t* mutex) {
  assert(*mutex == 1);
  return 0;
}
void rtos_delay_milliseconds(unsigned ms) { (void)ms; }
unsigned rtos_get_free_heap_size(void) { return 0; }
unsigned rtos_get_psram_free_heap_size(void) { return 0; }
int h2_bk7258_board_audio_power_set(int enabled) {
  assert(enabled == 0 || enabled == 1);
  ++power_calls[enabled];
  if (power_error) return power_error;
  power_on = enabled;
  return 0;
}
const h2_pal_mem_api_t* h2_bk7258_board_psram_allocator(void) { return NULL; }
const h2_pal_mem_api_t* h2_bk7258_board_default_allocator(void) { return NULL; }
const h2_pal_sync_api_t* h2_bk7258_board_sync_api(void) { return NULL; }
static int queue_create(void* user, const h2_pal_queue_config_t* cfg,
                        h2_pal_queue_t** out) {
  (void)user;
  assert(cfg->item_size == sizeof(h2_bk_mic_queue_frame_t));
  ++queue_creates;
  if (queue_error) return queue_error;
  *out = (h2_pal_queue_t*)&queue_token;
  return 0;
}
static int queue_reset(void* user, h2_pal_queue_t* queue) {
  (void)user;
  assert(queue == (h2_pal_queue_t*)&queue_token);
  return 0;
}
const h2_pal_queue_api_t* h2_bk7258_board_queue_api(void) {
  static const h2_pal_queue_vtable_t vt = {.create = queue_create,
                                           .reset = queue_reset};
  static const h2_pal_queue_api_t api = {.vtable = &vt};
  return &api;
}
static int task_start(void* user, const h2_pal_task_options_t* options,
                      h2_pal_task_entry_t entry, void* ctx,
                      h2_pal_task_t** out) {
  (void)user;
  (void)options;
  (void)entry;
  (void)ctx;
  if (task_error) return task_error;
  *out = (h2_pal_task_t*)&task_token;
  return 0;
}
const h2_pal_task_api_t* h2_bk7258_board_task_api(void) {
  static const h2_pal_task_vtable_t vt = {.start = task_start};
  static const h2_pal_task_api_t api = {.vtable = &vt};
  return &api;
}
audio_record_t* audio_record_create(int type, audio_record_cfg_t* cfg) {
  (void)type;
  (void)cfg;
  ++record_creates;
  return record_error ? NULL : &test_record;
}
int audio_record_open(audio_record_t* record) {
  assert(record == &test_record);
  return open_error;
}
int audio_record_close(audio_record_t* record) {
  assert(record == &test_record);
  return 0;
}
int audio_record_destroy(audio_record_t* record) {
  assert(record == &test_record);
  ++record_destroys;
  return destroy_error;
}
int audio_record_control(audio_record_t* record, int cmd) {
  assert(record == &test_record);
  return cmd == AUDIO_RECORD_PAUSE ? pause_error : open_error;
}
int audio_play_set_adc_gain(audio_record_t* record, int gain) {
  (void)gain;
  assert(record == &test_record);
  return gain_error;
}
int audio_record_read_data(audio_record_t* record, char* data, uint32_t size) {
  (void)record;
  (void)data;
  (void)size;
  return -1;
}
audio_play_t* audio_play_create(int type, audio_play_cfg_t* cfg) {
  (void)type;
  (void)cfg;
  return &test_play;
}
int audio_play_write_data(audio_play_t* play, char* data, uint32_t size) {
  (void)play;
  (void)data;
  (void)size;
  return -1;
}
int audio_play_open(audio_play_t* play) {
  assert(play == &test_play);
  return 0;
}
int audio_play_close(audio_play_t* play) {
  (void)play;
  return -1;
}
int audio_play_control(audio_play_t* play, int cmd) {
  assert(play == &test_play && cmd >= 0 && cmd <= AUDIO_PLAY_PAUSE);
  ++controls[cmd];
  return cmd == speaker_control_error ? -1 : 0;
}
int audio_play_set_volume(audio_play_t* play, int volume) {
  (void)play;
  (void)volume;
  return -1;
}

static void reset_test(void) {
  assert(live_context == NULL);
  s_audio_state = (h2_bk_audio_state_t){0};
  h2_bk7258_audio_config =
      (h2_bk7258_audio_config_t){.sample_rate = 16000,
                                 .frame_samples_per_channel = 320,
                                 .mic_channels = 2,
                                 .speaker_channels = 1,
                                 .bits_per_sample = 16,
                                 .default_mic_gain = 42,
                                 .mic_gain_max = 51,
                                 .frame_count = 4};
  osi_error = malloc_error = mutex_error = record_error = open_error =
      queue_error = task_error = gain_error = 0;
  power_error = pause_error = destroy_error = power_on = 0;
  speaker_control_error = -1;
  memset(power_calls, 0, sizeof(power_calls));
  memset(controls, 0, sizeof(controls));
  missing_buffer = -1;
  frame_samples = 320;
  allocations = frees = mutex_calls = record_creates = record_destroys =
      queue_creates = 0;
}
static void assert_not_ready(void) {
  assert(!s_audio_state.aec_initialized && !s_audio_state.mic_open);
  assert(s_audio_state.aec == NULL && s_audio_state.aec_ref == NULL &&
         s_audio_state.aec_mic == NULL && s_audio_state.aec_out == NULL);
  assert(allocations == frees);
}
static void finish(void) {
  assert(bk_audio_stop_mic(&s_audio_state) == H2_AUDIO_OK);
  bk_audio_release_aec(&s_audio_state);
  bk_audio_release_aec(&s_audio_state);
  assert(allocations == frees);
}
static void test_capability(void) {
  reset_test();
  h2_pal_audio_t* api = h2_bk7258_board_audio();
  h2_audio_info_t info;
  assert(h2_pal_audio_get_info(api, &info) == H2_AUDIO_OK);
  assert(info.aec_enabled && info.mic_format.sample_rate_hz == 16000 &&
         info.mic_format.channels == 1 &&
         info.mic_format.sample_format == H2_AUDIO_SAMPLE_S16LE &&
         info.mic_format.frame_samples_per_channel == 320);
  assert(!s_audio_state.aec_initialized && allocations == 0 &&
         record_creates == 0);
  assert(h2_pal_audio_set_aec_observer(api, NULL) == H2_AUDIO_ERR_UNSUPPORTED);
  for (unsigned variant = 0; variant < 6; ++variant) {
    reset_test();
    switch (variant) {
      case 0:
        h2_bk7258_audio_config.sample_rate = 48000;
        break;
      case 1:
        h2_bk7258_audio_config.frame_samples_per_channel = 160;
        break;
      case 2:
        h2_bk7258_audio_config.speaker_channels = 2;
        break;
      case 3:
        h2_bk7258_audio_config.mic_channels = 0;
        break;
      case 4:
        h2_bk7258_audio_config.mic_channels = 3;
        break;
      case 5:
        h2_bk7258_audio_config.bits_per_sample = 24;
        break;
    }
    assert(h2_pal_audio_get_info(api, &info) == H2_AUDIO_OK &&
           !info.aec_enabled);
    assert(bk_audio_init_aec(&s_audio_state) == H2_AUDIO_ERR_UNSUPPORTED);
    assert_not_ready();
  }
  reset_test();
  h2_bk7258_audio_config.sample_rate = 8000;
  h2_bk7258_audio_config.mic_channels = 2;
  assert(h2_pal_audio_get_info(api, &info) == H2_AUDIO_OK && info.aec_enabled);
  assert(bk_audio_init_aec(&s_audio_state) == H2_AUDIO_OK);
  finish();
}
static void test_failures_and_retry(void) {
  for (unsigned failure = 0; failure < 12; ++failure) {
    reset_test();
    int expected = H2_AUDIO_ERR_IO;
    switch (failure) {
      case 0:
        osi_error = -1;
        break;
      case 1:
        malloc_error = 1;
        expected = H2_AUDIO_ERR_NO_MEMORY;
        break;
      case 2:
        frame_samples = 160;
        expected = H2_AUDIO_ERR_UNSUPPORTED;
        break;
      case 3:
        missing_buffer = AEC_CTRL_CMD_GET_RX_BUF;
        break;
      case 4:
        missing_buffer = AEC_CTRL_CMD_GET_TX_BUF;
        break;
      case 5:
        missing_buffer = AEC_CTRL_CMD_GET_OUT_BUF;
        break;
      case 6:
        mutex_error = -1;
        break;
      case 7:
        record_error = 1;
        break;
      case 8:
        queue_error = H2_PAL_QUEUE_ERR_NO_MEMORY;
        expected = H2_AUDIO_ERR_NO_MEMORY;
        break;
      case 9:
        open_error = -1;
        break;
      case 10:
        task_error = -1;
        break;
      case 11:
        gain_error = -1;
        break;
    }
    h2_pal_audio_t* api = h2_bk7258_board_audio();
    assert(h2_pal_audio_start_mic(api) == expected);
    assert(!s_audio_state.mic_open);
    if (failure < 9) assert_not_ready();
    if (failure == 6) {
      assert(!s_audio_state.ref_mutex_initialized);
      assert(mutex_calls == 1 && allocations == 1 && frees == 1);
      assert(s_audio_state.record == &test_record &&
             s_audio_state.mic_queue != NULL);
    }
    unsigned before = allocations;
    osi_error = malloc_error = mutex_error = record_error = open_error =
        queue_error = task_error = gain_error = 0;
    missing_buffer = -1;
    frame_samples = 320;
    assert(h2_pal_audio_start_mic(api) == H2_AUDIO_OK);
    assert(s_audio_state.aec_initialized &&
           s_audio_state.ref_mutex_initialized && s_audio_state.mic_open);
    assert(allocations == before + (failure < 9 ? 1u : 0u));
    if (failure == 6)
      assert(record_creates == 1 && queue_creates == 1 && mutex_calls == 2);
    assert(bk_audio_init_aec(&s_audio_state) == H2_AUDIO_OK);
    int16_t raw[640] = {123, 999};
    assert(bk_audio_process_mic(&s_audio_state, raw, 320) == H2_AUDIO_OK);
    assert(s_audio_state.mic_processed_scratch[0] == 123);
    finish();
  }
}
static void test_power_and_retirement(void) {
  reset_test();
  h2_pal_audio_t* api = h2_bk7258_board_audio();
  power_error = H2_PAL_ERR_IO;
  assert(h2_pal_audio_start_mic(api) == power_error);
  assert(record_creates == 0 && !s_audio_state.opened);
  assert_not_ready();
  power_error = 0;
  assert(h2_pal_audio_start_mic(api) == H2_AUDIO_OK);
  for (unsigned failure = 0; failure < 3; ++failure) {
    if (failure == 0) pause_error = -1;
    if (failure == 1) destroy_error = -1;
    if (failure == 2)
      assert(h2_bk_audio_reader_enter(&s_audio_state.mic_open,
                                      &s_audio_state.mic_reading));
    assert(h2_pal_audio_stop_mic(api) == H2_AUDIO_ERR_IO);
    assert(s_audio_state.record == &test_record && s_audio_state.mic_retiring &&
           !s_audio_state.mic_open);
    unsigned creates = record_creates;
    assert(h2_pal_audio_start_mic(api) == H2_AUDIO_ERR_IO);
    assert(record_creates == creates);
    pause_error = destroy_error = 0;
    h2_bk_audio_reader_leave(&s_audio_state.mic_reading);
    assert(h2_pal_audio_start_mic(api) == H2_AUDIO_OK);
    assert(record_creates == creates + 1 && !s_audio_state.mic_retiring);
  }
  finish();
}
static void test_speaker_and_mic_order(void) {
  for (unsigned speaker_first = 0; speaker_first < 2; ++speaker_first) {
    reset_test();
    /* Avoid allocating an unrelated mixer; the provider still runs its actual
     * speaker start, stop, microphone, and task admission paths. */
    s_audio_state.mixer_initialized = true;
    h2_pal_audio_t* api = h2_bk7258_board_audio();
    if (!speaker_first) assert(h2_pal_audio_start_mic(api) == H2_AUDIO_OK);
    assert(h2_pal_audio_start_speaker(api) == H2_AUDIO_OK);
    assert(power_on && s_audio_state.speaker_open);
    unsigned off_calls = power_calls[0];
    if (speaker_first) assert(h2_pal_audio_start_mic(api) == H2_AUDIO_OK);
    assert(h2_pal_audio_stop_mic(api) == H2_AUDIO_OK);
    assert(h2_pal_audio_start_mic(api) == H2_AUDIO_OK);
    assert(power_on && power_calls[0] == off_calls);
    assert(h2_pal_audio_stop_speaker(api) == H2_AUDIO_OK);
    assert(!power_on && !s_audio_state.speaker_stopping);
    finish();
  }
}
static void test_speaker_stop_retry(void) {
  for (unsigned failure = 0; failure < 4; ++failure) {
    reset_test();
    s_audio_state.mixer_initialized = true;
    h2_pal_audio_t* api = h2_bk7258_board_audio();
    assert(h2_pal_audio_start_speaker(api) == H2_AUDIO_OK);
    if (failure == 0) atomic_store(&s_audio_state.playback_writing, true);
    if (failure == 1) speaker_control_error = AUDIO_PLAY_MUTE;
    if (failure == 2) speaker_control_error = AUDIO_PLAY_PAUSE;
    if (failure == 3) power_error = H2_PAL_ERR_IO;
    const int error = failure == 0 ? H2_PAL_ERR_TIMEOUT : H2_AUDIO_ERR_IO;
    assert(h2_pal_audio_stop_speaker(api) == error);
    assert(s_audio_state.speaker_stopping && !s_audio_state.speaker_open &&
           power_on);
    unsigned enables = power_calls[1];
    assert(h2_pal_audio_start_speaker(api) == error);
    assert(s_audio_state.speaker_stopping && power_calls[1] == enables);
    atomic_store(&s_audio_state.playback_writing, false);
    speaker_control_error = -1;
    power_error = 0;
    if (failure == 0) {
      assert(h2_pal_audio_stop_speaker(api) == H2_AUDIO_OK);
      assert(!s_audio_state.speaker_stopping && !power_on);
    }
    assert(h2_pal_audio_start_speaker(api) == H2_AUDIO_OK);
    assert(!s_audio_state.speaker_stopping && s_audio_state.speaker_open &&
           power_on);
    assert(controls[AUDIO_PLAY_PAUSE] > 0 && controls[AUDIO_PLAY_RESUME] == 1 &&
           controls[AUDIO_PLAY_UNMUTE] == 1);
    assert(h2_pal_audio_stop_speaker(api) == H2_AUDIO_OK);
    assert(!power_on && !s_audio_state.speaker_stopping);
    unsigned pauses = controls[AUDIO_PLAY_PAUSE];
    assert(h2_pal_audio_stop_speaker(api) == H2_AUDIO_OK &&
           controls[AUDIO_PLAY_PAUSE] == pauses);
  }
}
int main(void) {
  test_speaker_and_mic_order();
  test_speaker_stop_retry();
  test_power_and_retirement();
  test_capability();
  test_failures_and_retry();
  return 0;
}
