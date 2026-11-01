#include "h2_bk7258_board_private.h"
#include "h2_h202_board_test_sdk.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct test_audio_port {
  port_stream_func write;
  int type;
};
struct test_audio_element {
  audio_port_handle_t input, output, reference;
  bool task, running;
  int state;
};

static audio_element_handle_t capture, last_element;
static unsigned live_ports, live_elements, live_aec, live_mutexes;
static unsigned step, fail_step, aec_calls, stop_without_task, terminations;
static int run_error, resume_error, stop_error, wait_error, terminate_error;
static bool run_creates_task, run_delays_task, termination_finishes_later;
static uint64_t now_ms;

static bool fail(void) { return ++step == fail_step; }
static void* allocate(void* user, size_t size) {
  (void)user;
  if (fail()) return NULL;
  ++live_aec;
  return calloc(1, size);
}
static void release(void* user, void* ptr) {
  (void)user;
  assert(live_aec > 0);
  --live_aec;
  free(ptr);
}
const h2_pal_mem_api_t* h2_bk_platform_psram_allocator(void) {
  static const h2_pal_mem_vtable_t vtable = {.alloc = allocate,
                                             .free = release};
  static const h2_pal_mem_api_t api = {.vtable = &vtable};
  return &api;
}
static int monotonic(void* user, uint64_t* out) {
  (void)user;
  *out = now_ms;
  return 0;
}
const h2_pal_time_api_t* h2_bk_platform_time_api(void) {
  static const h2_pal_time_vtable_t vtable = {.get_monotonic_ms = monotonic};
  static const h2_pal_time_api_t api = {.vtable = &vtable};
  return &api;
}
void rtos_delay_milliseconds(unsigned ms) { now_ms += ms; }
int rtos_init_mutex(beken_mutex_t* mutex) {
  if (fail()) return -1;
  *mutex = malloc(sizeof(**mutex));
  assert(*mutex != NULL && pthread_mutex_init(*mutex, NULL) == 0);
  ++live_mutexes;
  return 0;
}
int rtos_lock_mutex(beken_mutex_t* mutex) {
  assert(*mutex != NULL);
  return pthread_mutex_lock(*mutex);
}
int rtos_unlock_mutex(beken_mutex_t* mutex) {
  return pthread_mutex_unlock(*mutex);
}
int rtos_deinit_mutex(beken_mutex_t* mutex) {
  assert(pthread_mutex_destroy(*mutex) == 0 && live_mutexes > 0);
  free(*mutex);
  *mutex = NULL;
  --live_mutexes;
  return 0;
}
unsigned aec_size(unsigned samples) {
  assert(samples == H2_H202_AUDIO_FRAME_SAMPLES);
  return sizeof(AECContext);
}
void aec_init(AECContext* aec, unsigned rate) {
  assert(aec != NULL && rate == H2_H202_AUDIO_RATE_HZ);
}
void aec_ctrl(AECContext* aec, uint32_t cmd, uintptr_t arg) {
  switch (cmd) {
    case AEC_CTRL_CMD_GET_FRAME_SAMPLE:
      *(uint32_t*)arg = H2_H202_AUDIO_FRAME_SAMPLES;
      break;
    case AEC_CTRL_CMD_GET_RX_BUF:
      *(uintptr_t*)arg = (uintptr_t)aec->ref;
      break;
    case AEC_CTRL_CMD_GET_TX_BUF:
      *(uintptr_t*)arg = (uintptr_t)aec->mic;
      break;
    case AEC_CTRL_CMD_GET_OUT_BUF:
      *(uintptr_t*)arg = (uintptr_t)aec->out;
      break;
    default:
      break;
  }
}
void aec_proc(AECContext* aec, int16_t* ref, int16_t* mic, int16_t* out) {
  assert(ref == aec->ref && mic == aec->mic && out == aec->out);
  ++aec_calls;
  for (unsigned i = 0; i < H2_H202_AUDIO_FRAME_SAMPLES; ++i)
    out[i] = mic[i] - ref[i];
}
int bk_audio_osi_funcs_init(void) { return fail() ? -1 : 0; }
audio_port_handle_t audio_port_init(audio_port_cfg_t* config) {
  if (fail()) return NULL;
  audio_port_handle_t port = calloc(1, sizeof(*port));
  assert(port != NULL);
  port->write = config->write;
  ++live_ports;
  return port;
}
audio_port_handle_t ringbuf_port_init(ringbuf_port_cfg_t* config) {
  assert(config->ringbuf_size == H2_H202_AUDIO_FRAME_SAMPLES * 2 * 4);
  audio_port_cfg_t cb = {0};
  audio_port_handle_t port = audio_port_init(&cb);
  if (port != NULL) port->type = PORT_TYPE_RB;
  return port;
}
int audio_port_set_type(audio_port_handle_t port, int type) {
  if (fail()) return -1;
  port->type = type;
  return 0;
}
int audio_port_deinit(audio_port_handle_t port) {
  assert(port != NULL && live_ports > 0);
  --live_ports;
  free(port);
  return 0;
}
int audio_port_write(audio_port_handle_t port, char* buffer, int bytes,
                     TickType_t ticks) {
  if (port->write != NULL) return port->write(port, buffer, bytes, ticks, NULL);
  assert(port->type == PORT_TYPE_RB);
  return bytes;
}
int audio_port_get_filled_size(audio_port_handle_t port) {
  assert(port != NULL);
  return 0;
}
static audio_element_handle_t element(void) {
  if (fail()) return NULL;
  audio_element_handle_t el = calloc(1, sizeof(*el));
  assert(el != NULL);
  ++live_elements;
  last_element = el;
  return el;
}
audio_element_handle_t onboard_dual_dmic_mic_stream_init(
    onboard_dual_dmic_mic_stream_cfg_t* config) {
  assert(config->dual_dmic == 1 && config->dual_dmic_sgl_out == 0 &&
         config->ref_mode == 1 && config->multi_out_port_num == 1);
  assert(config->frame_size == H2_H202_AUDIO_FRAME_SAMPLES * 2);
  capture = element();
  return capture;
}
audio_element_handle_t onboard_speaker_stream_init(
    onboard_speaker_stream_cfg_t* config) {
  assert(config->frame_size == H2_H202_AUDIO_FRAME_SAMPLES * 2 &&
         config->pa_ctrl_gpio == H2_H202_PA_ENABLE_GPIO);
  return element();
}
int audio_element_set_output_port(audio_element_handle_t el,
                                  audio_port_handle_t port) {
  if (port != NULL && fail()) return -1;
  el->output = port;
  return 0;
}
int audio_element_set_multi_output_port(audio_element_handle_t el,
                                        audio_port_handle_t port, int index) {
  assert(index == 0);
  if (fail()) return -1;
  el->reference = port;
  return 0;
}
int audio_element_set_input_port(audio_element_handle_t el,
                                 audio_port_handle_t port) {
  if (fail()) return -1;
  el->input = port;
  return 0;
}
int audio_element_set_output_timeout(audio_element_handle_t el,
                                     TickType_t ticks) {
  assert(el != NULL && ticks == 100);
  return fail() ? -1 : 0;
}
int audio_element_set_input_timeout(audio_element_handle_t el,
                                    TickType_t ticks) {
  return audio_element_set_output_timeout(el, ticks);
}
int audio_element_get_state(audio_element_handle_t el) { return el->state; }
int audio_element_run(audio_element_handle_t el) {
  el->task = run_error == 0 || run_creates_task;
  el->state = run_error && !run_delays_task && !run_creates_task
                  ? AEL_STATE_ERROR
                  : AEL_STATE_INIT;
  return run_error;
}
int audio_element_resume(audio_element_handle_t el, float threshold,
                         TickType_t ticks) {
  assert(el->task && threshold == 0 && ticks == 1000);
  el->running = true;
  return resume_error;
}
int audio_element_stop(audio_element_handle_t el) {
  if (!el->task) {
    ++stop_without_task;
    return -1; /* SDK behavior after thread creation failure. */
  }
  if (stop_error) return stop_error;
  el->running = false;
  return 0;
}
int audio_element_wait_for_stop_ms(audio_element_handle_t el,
                                   TickType_t ticks) {
  assert(el != NULL && ticks == 1000);
  return wait_error;
}
int audio_element_terminate_with_ticks(audio_element_handle_t el,
                                       TickType_t ticks) {
  assert(ticks == 1000);
  ++terminations;
  if (terminate_error) {
    if (termination_finishes_later) el->task = el->running = false;
    return terminate_error;
  }
  el->task = false;
  el->running = false;
  return 0;
}
int audio_element_deinit(audio_element_handle_t el) {
  assert(!el->task && !el->running && live_elements > 0);
  /* Model SDK ownership: primary CB is auto-freed, multi-output is borrowed. */
  if (el->output != NULL && el->output->type == PORT_TYPE_CB)
    audio_port_deinit(el->output);
  if (el == capture) capture = NULL;
  --live_elements;
  free(el);
  return 0;
}

static const h2_pal_audio_api_t* audio;
static void clean(void) {
  assert(h2_h202_audio_deinit() == 0);
  assert(live_ports == 0 && live_elements == 0 && live_aec == 0 &&
         live_mutexes == 0 && capture == NULL);
}
static void errors_clear(void) {
  run_error = resume_error = stop_error = wait_error = terminate_error = 0;
  run_creates_task = run_delays_task = termination_finishes_later = false;
  fail_step = step = 0;
}
static int start(bool mic) {
  return mic ? audio->vtable->start_mic(NULL)
             : audio->vtable->start_speaker(NULL);
}
static void test_start_failures(void) {
  for (unsigned mic = 0; mic < 2; ++mic) {
    errors_clear();
    assert(start(mic) == 0);
    unsigned steps = step;
    clean();
    for (unsigned n = 1; n <= steps; ++n) {
      errors_clear();
      fail_step = n;
      assert(start(mic) != 0);
      fail_step = 0;
      clean();
      assert(start(mic) == 0);
      clean();
    }
    for (unsigned task = 0; task < 2; ++task) {
      errors_clear();
      run_error = -1;
      run_creates_task = task;
      assert(start(mic) == H2_PAL_ERR_IO);
      clean();
      errors_clear();
      assert(start(mic) == 0);
      clean();
    }
    errors_clear();
    resume_error = -1;
    assert(start(mic) == H2_PAL_ERR_IO);
    clean();
    errors_clear();
    assert(start(mic) == 0);
    stop_error = -1;
    clean(); /* Stop-command failure must still attempt the bounded join. */
    errors_clear();
    run_error = terminate_error = -1;
    run_creates_task = true;
    assert(start(mic) == H2_PAL_ERR_TIMEOUT);
    assert(live_elements == 1 && live_ports > 0);
    if (mic) assert(live_aec == 2 && live_mutexes == 1);
    errors_clear();
    clean();
    run_error = -1;
    run_delays_task = true;
    assert(start(mic) == H2_PAL_ERR_TIMEOUT);
    assert(live_elements == 1 && live_ports > 0 && !last_element->task);
    assert(h2_h202_audio_deinit() == H2_PAL_ERR_TIMEOUT);
    last_element->task = true; /* Delayed SDK task finally announces itself. */
    errors_clear();
    clean();
    assert(start(mic) == 0);
    terminate_error = -1;
    termination_finishes_later = true;
    assert(h2_h202_audio_deinit() == H2_PAL_ERR_TIMEOUT);
    assert(live_elements == 1 && !last_element->task);
    errors_clear();
    clean(); /* Retry tolerates SDK stop returning FAIL after task exit. */
    assert(start(mic) == 0);
    wait_error = -1;
    assert(h2_h202_audio_deinit() == H2_PAL_ERR_TIMEOUT);
    assert(live_elements == 1 && live_ports > 0);
    errors_clear();
    clean();
  }
  assert(stop_without_task > 0 && terminations > 0);
}

static int16_t raw[H2_H202_AUDIO_FRAME_SAMPLES * 2];
static int16_t ref[H2_H202_AUDIO_FRAME_SAMPLES];
static int16_t out[H2_H202_AUDIO_FRAME_SAMPLES * 2];
static void samples(unsigned sequence) {
  for (unsigned i = 0; i < H2_H202_AUDIO_FRAME_SAMPLES; ++i) {
    ref[i] = sequence * 3;
    raw[i * 2] = sequence * 5 + 1;
    raw[i * 2 + 1] = sequence * 7 + 2;
  }
}
static int reference(unsigned sequence) {
  samples(sequence);
  return audio_port_write(capture->reference, (char*)ref, sizeof(ref), 0);
}
static int microphone(void) {
  return audio_port_write(capture->output, (char*)raw, sizeof(raw), 100);
}
static void produce(unsigned sequence) {
  assert(reference(sequence) == (int)sizeof(ref));
  assert(microphone() == (int)sizeof(raw));
}
static int read_frame(unsigned sequence, unsigned timeout) {
  h2_audio_frame_t frame = {.data = out, .capacity = sizeof(out), .bytes = 99};
  int rc = audio->vtable->mic_read(NULL, &frame, timeout);
  if (rc != 0) {
    assert(frame.bytes == 0);
    return rc;
  }
  assert(frame.bytes == sizeof(out) && frame.channels == 2 &&
         frame.sample_rate_hz == H2_H202_AUDIO_RATE_HZ &&
         frame.samples_per_channel == H2_H202_AUDIO_FRAME_SAMPLES);
  for (unsigned i = 0; i < H2_H202_AUDIO_FRAME_SAMPLES; ++i) {
    assert(out[i * 2] == (int16_t)(sequence * 2 + 1));
    assert(out[i * 2 + 1] == (int16_t)(sequence * 4 + 2));
  }
  return rc;
}
static void restart(void) {
  clean();
  assert(start(true) == 0);
}
static void test_pairing(void) {
  errors_clear();
  assert(start(true) == 0);
  assert(read_frame(0, 7) == H2_PAL_ERR_WOULD_BLOCK && now_ms == 7);
  produce(1);
  assert(reference(2) == (int)sizeof(ref));
  assert(read_frame(1, 0) == 0); /* Consumer moves head with ref reserved. */
  unsigned calls = aec_calls;
  assert(read_frame(0, 0) == H2_PAL_ERR_WOULD_BLOCK && aec_calls == calls);
  assert(microphone() == (int)sizeof(raw));
  assert(read_frame(2, 0) == 0);
  for (unsigned sequence = 3; sequence < 50; ++sequence) {
    produce(sequence);
    assert(read_frame(sequence, 0) == 0);
  }
  for (unsigned sequence = 50; sequence < 54; ++sequence) produce(sequence);
  /* SDK ignores rejected ref and still attempts mic. Both must fail even if
   * a consumer appears between them; no old mic/ref pair reaches AEC. */
  assert(reference(54) < 0);
  calls = aec_calls;
  assert(read_frame(50, 0) == H2_PAL_ERR_IO && aec_calls == calls);
  assert(microphone() < 0);
  assert(read_frame(50, 0) == H2_PAL_ERR_IO);
  restart();
  produce(60);
  assert(read_frame(60, 0) == 0);
  assert(microphone() < 0); /* A mic cannot borrow the previous ref. */
  assert(read_frame(0, 0) == H2_PAL_ERR_IO);
  restart();
  assert(reference(61) == (int)sizeof(ref));
  assert(reference(62) < 0); /* Missing mic cannot shift the next ref. */
  assert(microphone() < 0 && read_frame(0, 0) == H2_PAL_ERR_IO);
  restart();
  assert(audio_port_write(capture->reference, (char*)ref, sizeof(ref) - 2, 0) <
         0);
  assert(microphone() < 0 && read_frame(0, 0) == H2_PAL_ERR_IO);
  restart();
  assert(reference(63) == (int)sizeof(ref));
  assert(audio_port_write(capture->output, (char*)raw, sizeof(raw) - 2, 0) < 0);
  assert(read_frame(0, 0) == H2_PAL_ERR_IO);
  restart();
  assert(reference(64) == (int)sizeof(ref));
  restart(); /* A reserved half-frame is discarded on stop. */
  produce(65);
  memset(raw, 0, sizeof(raw));
  memset(ref, 0, sizeof(ref)); /* Queue owns copies, never SDK buffers. */
  assert(read_frame(65, 0) == 0);
  clean();
}
static pthread_mutex_t producer_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t producer_ready = PTHREAD_COND_INITIALIZER;
static unsigned producer_phase;

static void phase_set(unsigned phase) {
  assert(pthread_mutex_lock(&producer_lock) == 0);
  producer_phase = phase;
  assert(pthread_cond_broadcast(&producer_ready) == 0);
  assert(pthread_mutex_unlock(&producer_lock) == 0);
}
static void phase_wait(unsigned phase) {
  assert(pthread_mutex_lock(&producer_lock) == 0);
  while (producer_phase != phase)
    assert(pthread_cond_wait(&producer_ready, &producer_lock) == 0);
  assert(pthread_mutex_unlock(&producer_lock) == 0);
}
static void* producer(void* user) {
  (void)user;
  for (unsigned i = 1; i <= 200; ++i) {
    phase_wait(i * 4);
    assert(reference(i) == (int)sizeof(ref));
    phase_set(i * 4 + 1);
    phase_wait(i * 4 + 2);
    assert(microphone() == (int)sizeof(raw));
    phase_set(i * 4 + 3);
  }
  return NULL;
}
static void test_concurrent_pairing(void) {
  errors_clear();
  assert(start(true) == 0);
  pthread_t thread;
  producer_phase = 0;
  assert(pthread_create(&thread, NULL, producer, NULL) == 0);
  for (unsigned i = 1; i <= 200; ++i) {
    phase_set(i * 4);
    phase_wait(i * 4 + 1);
    /* Different tasks race at the SDK callback boundary. Pending reference
     * must stay private while the PAL consumer tries to dequeue. */
    assert(read_frame(0, 0) == H2_PAL_ERR_WOULD_BLOCK);
    phase_set(i * 4 + 2);
    phase_wait(i * 4 + 3);
    assert(read_frame(i, 0) == 0);
  }
  assert(pthread_join(thread, NULL) == 0);
  clean();
}
static unsigned observed_captures, observed_playbacks, observed_errors;
static void on_capture(void* user, const h2_audio_aec_frame_t* frame) {
  assert(user == &observed_captures);
  unsigned lane = observed_captures % 2;
  assert(frame->sequence == observed_captures);
  assert(frame->microphone_lane == lane && frame->reference_lane == 2 &&
         frame->microphone_mask == 3);
  assert(frame->raw->channels == 3 && frame->reference_input->channels == 1 &&
         frame->processed->channels == 1);
  const int16_t* raw_lanes = frame->raw->data;
  const int16_t* reference_input = frame->reference_input->data;
  const int16_t* processed = frame->processed->data;
  /* produce(70): left 351, right 492, reference 210; fake AEC subtracts. */
  assert(raw_lanes[0] == 351 && raw_lanes[1] == 492 && raw_lanes[2] == 210);
  assert(reference_input[0] == 210);
  assert(processed[0] == (lane == 0 ? 141 : 282));
  ++observed_captures;
}
static void on_playback(void* user, const h2_audio_frame_t* frame) {
  assert(user == &observed_captures);
  assert(frame->channels == 1 &&
         frame->bytes == H2_H202_AUDIO_FRAME_SAMPLES * 2);
  ++observed_playbacks;
}
static void on_error(void* user, int capture, int result) {
  assert(user == &observed_captures && capture == 1 && result == H2_PAL_ERR_IO);
  ++observed_errors;
}
static void test_observer(void) {
  errors_clear();
  const h2_audio_aec_observer_t observer = {.user = &observed_captures,
                                            .on_capture = on_capture,
                                            .on_playback = on_playback,
                                            .on_error = on_error};
  assert(start(true) == 0);
  assert(h2_pal_audio_set_aec_observer(audio, &observer) ==
         H2_PAL_ERR_INVALID_STATE);
  clean();
  assert(h2_pal_audio_set_aec_observer(audio, &observer) == 0);
  h2_audio_info_t info;
  assert(audio->vtable->get_info(NULL, &info) == 0 && info.aec_enabled);
  assert(start(true) == 0);
  produce(70);
  assert(read_frame(70, 0) == 0 && observed_captures == 2);
  assert(microphone() < 0); /* Mic without reference latches a fault. */
  assert(read_frame(0, 0) == H2_PAL_ERR_IO && observed_errors == 1);
  clean();
  assert(start(false) == 0);
  const h2_audio_track_config_t config = {
      .format = {.sample_rate_hz = H2_H202_AUDIO_RATE_HZ,
                 .frame_samples_per_channel = H2_H202_AUDIO_FRAME_SAMPLES,
                 .channels = 1,
                 .sample_format = H2_AUDIO_SAMPLE_S16LE},
      .volume_factor_milli = 1000};
  h2_pal_audio_track_t* track = NULL;
  assert(audio->vtable->create_track(NULL, &config, &track) == 0);
  int16_t pcm[H2_H202_AUDIO_FRAME_SAMPLES] = {0};
  const h2_audio_frame_t frame = {
      .data = pcm,
      .capacity = sizeof(pcm),
      .bytes = sizeof(pcm),
      .sample_rate_hz = H2_H202_AUDIO_RATE_HZ,
      .samples_per_channel = H2_H202_AUDIO_FRAME_SAMPLES,
      .channels = 1,
      .sample_format = H2_AUDIO_SAMPLE_S16LE};
  assert(track->write(track, &frame, 10) == 0 && observed_playbacks == 1);
  assert(track->close(track) == 0);
  assert(h2_pal_audio_set_aec_observer(audio, NULL) ==
         H2_PAL_ERR_INVALID_STATE);
  clean();
  assert(h2_pal_audio_set_aec_observer(audio, NULL) == 0);
}
int main(void) {
  audio = h2_h202_audio_api();
  test_start_failures();
  test_pairing();
  test_concurrent_pairing();
  test_observer();
  return 0;
}
