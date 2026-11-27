#include "h2_bk7258_board_private.h"
#include "h2_h202_board_test_sdk.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct test_audio_port {
  port_stream_func write;
  int type;
  unsigned used;
  char queued[H2_H202_AUDIO_FRAME_SAMPLES * 2 * 4];
};
struct test_audio_element {
  audio_port_handle_t input, output, reference;
  bool task, running;
  int state;
  int (*open)(audio_element_handle_t);
  int (*process)(audio_element_handle_t, char*, int);
};

static audio_element_handle_t capture, playback, last_element;
static unsigned live_ports, live_elements, live_aec, live_mutexes;
static unsigned step, fail_step, aec_calls, stop_without_task, terminations;
static int run_error, resume_error, stop_error, wait_error, terminate_error;
static bool run_creates_task, run_delays_task, termination_finishes_later;
static uint64_t now_ms;
static int input_result;

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

/* PA enable line: rising edges since the last 2 ms low hold select the mode. */
static bool pa_high, pa_irq_off;
static unsigned pa_edges, pa_us;
static uint64_t pa_low_since;
int gpio_dev_unmap(gpio_id_t pin) {
  assert(pin == H2_H202_PA_ENABLE_GPIO);
  return 0;
}
int bk_gpio_set_config(gpio_id_t pin, const gpio_config_t* config) {
  assert(pin == H2_H202_PA_ENABLE_GPIO &&
         config->io_mode == GPIO_OUTPUT_ENABLE);
  return 0;
}
int bk_gpio_set_output_low(gpio_id_t pin) {
  assert(pin == H2_H202_PA_ENABLE_GPIO);
  if (pa_high) pa_low_since = now_ms;
  pa_high = false;
  return 0;
}
int bk_gpio_set_output_high(gpio_id_t pin) {
  assert(pin == H2_H202_PA_ENABLE_GPIO);
  if (!pa_high) {
    if (now_ms - pa_low_since >= 2u) pa_edges = 0; /* Shutdown resets. */
    assert(pa_edges == 0u || pa_irq_off);          /* Pulses stay tight. */
    ++pa_edges;
  }
  pa_high = true;
  return 0;
}
void bk_delay_us(uint32_t us) {
  assert(us >= 1u && us <= 10u);
  pa_us += us;
}
uint32_t rtos_disable_int(void) {
  pa_irq_off = true;
  return 7u;
}
void rtos_enable_int(uint32_t level) {
  assert(level == 7u && pa_irq_off);
  pa_irq_off = false;
}
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
unsigned aec_size(unsigned delay_samples) {
  /* The SDK initializes an 800-point delay ring independently of the
   * 320-sample frame size. Underallocating corrupts the next AEC context. */
  assert(delay_samples >= 800u);
  return sizeof(AECContext) + delay_samples * sizeof(int16_t);
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
  unsigned free_bytes = sizeof(port->queued) - port->used;
  unsigned copied = (unsigned)bytes < free_bytes ? (unsigned)bytes : free_bytes;
  memcpy(port->queued + port->used, buffer, copied);
  port->used += copied;
  return (int)copied;
}
int audio_port_get_filled_size(audio_port_handle_t port) {
  assert(port != NULL);
  return (int)port->used;
}
int audio_element_input(audio_element_handle_t element, char* buffer,
                        int length) {
  if (input_result != 0) return input_result;
  audio_port_handle_t port = element->input;
  assert(port != NULL && port->type == PORT_TYPE_RB);
  unsigned copied =
      (unsigned)length < port->used ? (unsigned)length : port->used;
  if (copied == 0u) return AEL_IO_TIMEOUT;
  memcpy(buffer, port->queued, copied);
  port->used -= copied;
  memmove(port->queued, port->queued + copied, port->used);
  return (int)copied;
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
audio_element_handle_t audio_element_init(audio_element_cfg_t* config) {
  assert(config->buffer_len == H2_H202_AUDIO_FRAME_SAMPLES * 2 &&
         config->process != NULL && config->task_core == 1u &&
         config->task_prio == 5u && config->task_stack == 8192u);
  playback = element();
  if (playback != NULL) {
    playback->open = config->open;
    playback->process = config->process;
  }
  return playback;
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
  assert(el == playback && ticks == 0u);
  return fail() ? -1 : 0;
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
  /* Match the SDK: a NULL open returns success without advancing INIT. */
  if (el->open != NULL) {
    if (el->open(el) != 0) return -1;
    el->state = AEL_STATE_RUNNING;
  }
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
  if (el == playback) playback = NULL;
  --live_elements;
  free(el);
  return 0;
}

/* The fake distinguishes producer enqueue from worker consumption and the
 * verified DAC-ring write. No DMA completion is implied by enqueueing. */
static bool dac_live, dac_running, dma_claimed, dma_live, dma_running;
static unsigned dma_buffers, semaphores, dac_writes;
static int dma_stop_error, dac_stop_error;
static bool short_dac_write;
static dma_isr_t dma_finish;
static int16_t last_dac_pcm[H2_H202_AUDIO_FRAME_SAMPLES];
int rtos_init_semaphore(beken_semaphore_t* out, unsigned count) {
  assert(count == 1u);
  if (fail()) return -1;
  *out = calloc(1, sizeof(bool));
  assert(*out != NULL);
  ++semaphores;
  return 0;
}
int rtos_set_semaphore(beken_semaphore_t* semaphore) {
  assert(*semaphore != NULL);
  *(bool*)*semaphore = true;
  return 0;
}
int rtos_get_semaphore(beken_semaphore_t* semaphore, unsigned timeout) {
  assert(*semaphore != NULL && timeout == 1000u);
  if (!*(bool*)*semaphore) return -1;
  *(bool*)*semaphore = false;
  return 0;
}
int rtos_deinit_semaphore(beken_semaphore_t* semaphore) {
  assert(!dma_live && !dma_running && semaphores > 0u);
  free(*semaphore);
  *semaphore = NULL;
  --semaphores;
  return 0;
}
int bk_aud_dac_init(aud_dac_config_t* config) {
  assert(!dac_live && config->samp_rate == 16000u && config->dac_gain == 0x2du);
  if (fail()) return -1;
  dac_live = true;
  return 0;
}
int bk_aud_dac_deinit(void) {
  assert(dac_live && !dac_running);
  dac_live = false;
  return 0;
}
int bk_aud_dac_start(void) {
  assert(dac_live && dma_running);
  dac_running = true;
  return fail() ? -1 : 0;
}
int bk_aud_dac_stop(void) {
  assert(dac_live && !dma_running);
  if (dac_stop_error) return dac_stop_error;
  dac_running = false;
  return 0;
}
int bk_aud_dac_mute(void) { return fail() ? -1 : 0; }
int bk_aud_dac_unmute(void) { return fail() ? -1 : 0; }
int bk_aud_dac_get_fifo_addr(uint32_t* out) {
  *out = 0x1000u;
  return fail() ? -1 : 0;
}
int bk_aud_set_ana_dac_gain(uint8_t gain) {
  assert(gain == 0x0au);
  return fail() ? -1 : 0;
}
int bk_dma_driver_init(void) { return fail() ? -1 : 0; }
dma_id_t bk_dma_alloc(uint16_t user) {
  assert(user == DMA_DEV_AUDIO && !dma_claimed);
  if (fail()) return DMA_ID_MAX;
  dma_claimed = true;
  return 0;
}
int bk_dma_free(uint16_t user, dma_id_t id) {
  assert(user == DMA_DEV_AUDIO && id == 0u && dma_claimed && !dma_live);
  dma_claimed = false;
  return 0;
}
int bk_dma_init(dma_id_t id, const dma_config_t* config) {
  assert(id == 0u && dma_claimed && !dma_live);
  assert(config->mode == DMA_WORK_MODE_REPEAT && config->chan_prio == 1u);
  assert(config->src.width == DMA_DATA_WIDTH_32BITS &&
         config->dst.width == DMA_DATA_WIDTH_16BITS &&
         config->src.end_addr - config->src.start_addr == 1288u &&
         config->dst.start_addr == 0x1000u && config->dst.end_addr == 0x1004u);
  dma_live = true; /* Partial init is still owned if it reports failure. */
  return fail() ? -1 : 0;
}
int bk_dma_deinit(dma_id_t id) {
  assert(id == 0u && dma_live && !dma_running);
  dma_live = false;
  dma_finish = NULL;
  return 0;
}
int bk_dma_start(dma_id_t id) {
  assert(id == 0u && dma_live);
  dma_running = true;
  return fail() ? -1 : 0;
}
int bk_dma_stop(dma_id_t id) {
  assert(id == 0u && dma_live);
  if (dma_stop_error) return dma_stop_error;
  dma_running = false;
  return 0;
}
uint32_t bk_dma_get_enable_status(dma_id_t id) {
  assert(id == 0u && dma_live);
  return dma_running;
}
int bk_dma_set_transfer_len(dma_id_t id, uint32_t length) {
  assert(id == 0u && dma_live && length == 640u);
  return fail() ? -1 : 0;
}
int bk_dma_register_isr(dma_id_t id, dma_isr_t half, dma_isr_t finish) {
  assert(id == 0u && dma_live && half == NULL);
  dma_finish = finish;
  return finish != NULL && fail() ? -1 : 0;
}
int bk_dma_enable_finish_interrupt(dma_id_t id) {
  assert(id == 0u && dma_live);
  return fail() ? -1 : 0;
}
int bk_dma_disable_finish_interrupt(dma_id_t id) {
  assert(id == 0u && dma_live && !dma_running);
  return 0;
}
void* audio_dma_mem_calloc(uint32_t count, uint32_t size) {
  assert(count * size == 1288u);
  if (fail()) return NULL;
  ++dma_buffers;
  return calloc(count, size);
}
void audio_dma_mem_free(void* ptr) {
  assert(dma_buffers > 0u && !dma_live && !dma_running);
  --dma_buffers;
  free(ptr);
}
void ring_buffer_init(RingBufferContext* rb, uint8_t* address,
                      uint32_t capacity, dma_id_t id, uint32_t direction) {
  assert(id == 0u && capacity == 1288u && direction == RB_DMA_TYPE_READ);
  *rb = (RingBufferContext){
      .address = address, .capacity = capacity, .dma_id = id};
}
uint32_t ring_buffer_write(RingBufferContext* rb, uint8_t* bytes,
                           uint32_t size) {
  assert(rb->dma_id == 0u && dma_live);
  if (!dma_running) {
    assert(size == 1280u);
    return fail() ? 0u : size;
  }
  assert(size == 640u);
  if (short_dac_write) return size - 2u;
  memcpy(last_dac_pcm, bytes, size);
  ++dac_writes;
  return size;
}

static int consume_speaker(bool dma_signal) {
  assert(playback != NULL && playback->process != NULL);
  if (!playback->running || playback->state != AEL_STATE_RUNNING)
    return AEL_IO_FAIL;
  if (dma_signal) {
    assert(dma_finish != NULL);
    dma_finish(0);
  }
  int16_t frame[H2_H202_AUDIO_FRAME_SAMPLES];
  return playback->process(playback, (char*)frame, sizeof(frame));
}

static const h2_pal_audio_api_t* audio;
static void clean(void) {
  assert(h2_h202_audio_deinit() == 0);
  assert(live_ports == 0 && live_elements == 0 && live_aec == 0 &&
         live_mutexes == 0 && capture == NULL && playback == NULL);
  assert(!dac_live && !dac_running && !dma_claimed && !dma_live &&
         !dma_running && dma_buffers == 0u && semaphores == 0u);
}
static void errors_clear(void) {
  run_error = resume_error = stop_error = wait_error = terminate_error = 0;
  run_creates_task = run_delays_task = termination_finishes_later = false;
  fail_step = step = 0;
  dma_stop_error = dac_stop_error = 0;
  short_dac_write = false;
  input_result = 0;
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
    /* A failed start is not proof that its pending worker is stopped. */
    assert(h2_pal_audio_set_aec_observer(audio, NULL) ==
           H2_PAL_ERR_INVALID_STATE);
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
  now_ms = 0; /* Earlier speaker stops waited for the PA. */
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
  /* Fill all eight queued frames. */
  for (unsigned sequence = 50; sequence < 58; ++sequence) produce(sequence);
  /* SDK ignores rejected ref and still attempts mic. Both must fail even if
   * a consumer appears between them; no old mic/ref pair reaches AEC. */
  assert(reference(58) < 0);
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
static int last_error_capture;
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
  assert(dac_writes > 0u &&
         memcmp(frame->data, last_dac_pcm, frame->bytes) == 0);
  ++observed_playbacks;
}
static void on_error(void* user, int capture, int result) {
  assert(user == &observed_captures && result == H2_PAL_ERR_IO);
  last_error_capture = capture;
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
  for (unsigned i = 0; i < H2_H202_AUDIO_FRAME_SAMPLES; ++i) pcm[i] = 1000;
  unsigned writes = dac_writes;
  assert(track->write(track, &frame, 10) == 0 && observed_playbacks == 0);
  assert(dac_writes == writes); /* Producer success only means enqueued. */
  memset(pcm, 0, sizeof(pcm));  /* Queue owns the scaled bytes. */
  assert(track->drain(track, 0) == H2_PAL_ERR_TIMEOUT);
  assert(consume_speaker(true) == (int)sizeof(pcm));
  assert(observed_playbacks == 1 && dac_writes == writes + 1u);
  for (unsigned i = 0; i < H2_H202_AUDIO_FRAME_SAMPLES; ++i)
    assert(last_dac_pcm[i] == 350);
  assert(track->drain(track, 100) == 0);
  /* Enqueue still succeeds when the later DAC write fails. Consumption must
   * report an error, never playback, and the failure remains visible to drain.
   */
  assert(track->write(track, &frame, 10) == 0);
  short_dac_write = true;
  assert(consume_speaker(true) == AEL_IO_FAIL);
  assert(observed_playbacks == 1 && observed_errors == 2 &&
         last_error_capture == 0);
  assert(track->drain(track, 100) == H2_PAL_ERR_IO);
  assert(track->write(track, &frame, 10) == H2_PAL_ERR_IO);
  assert(track->close(track) == 0);
  assert(h2_pal_audio_set_aec_observer(audio, NULL) ==
         H2_PAL_ERR_INVALID_STATE);
  clean();
  errors_clear();
  assert(start(false) == 0);
  assert(audio->vtable->create_track(NULL, &config, &track) == 0);
  assert(track->write(track, &frame, 10) == 0);
  assert(consume_speaker(false) == AEL_IO_FAIL); /* DMA stopped signaling. */
  assert(observed_playbacks == 1 && observed_errors == 3 &&
         last_error_capture == 0);
  assert(track->close(track) == 0);
  clean();
  errors_clear();
  assert(start(false) == 0);
  assert(audio->vtable->create_track(NULL, &config, &track) == 0);
  assert(track->write(track, &frame, 10) == 0);
  dma_running = false; /* A late signal cannot make a stopped DMA valid. */
  assert(consume_speaker(true) == AEL_IO_FAIL);
  assert(observed_playbacks == 1 && observed_errors == 4);
  assert(track->close(track) == 0);
  clean();
  errors_clear();
  assert(start(false) == 0);
  assert(audio->vtable->create_track(NULL, &config, &track) == 0);
  input_result = AEL_IO_ABORT; /* A normal SDK stop is not a transport fault. */
  assert(consume_speaker(true) == AEL_IO_ABORT);
  assert(observed_playbacks == 1 && observed_errors == 4);
  assert(track->close(track) == 0);
  clean();
  assert(h2_pal_audio_set_aec_observer(audio, NULL) == 0);
}
static void test_dac_cleanup(void) {
  errors_clear();
  assert(start(false) == 0);
  dma_stop_error = -1;
  assert(h2_h202_audio_deinit() == H2_PAL_ERR_IO);
  assert(playback == NULL && dma_running && dma_live && semaphores == 1u &&
         dma_buffers == 1u && live_ports == 1u);
  assert(start(false) != H2_PAL_OK);
  dma_stop_error = 0;
  dac_stop_error = -1;
  assert(h2_h202_audio_deinit() == H2_PAL_ERR_IO);
  assert(!dma_running && dac_running && semaphores == 1u && dma_buffers == 1u);
  dac_stop_error = 0;
  clean();
}
static void test_pa_mode(void) {
  errors_clear();
  pa_high = false;
  pa_low_since = now_ms;
  assert(start(false) == 0);
  /* Four rising edges after the shutdown hold: AW8733A mode 4, left on. */
  assert(pa_high && pa_edges == H2_H202_PA_MODE && !pa_irq_off);
  clean();
  assert(!pa_high);
  /* A restart selects the mode again rather than counting on. */
  assert(start(false) == 0 && pa_high && pa_edges == H2_H202_PA_MODE);
  clean();
  assert(!pa_high);
}
int main(void) {
  audio = h2_h202_audio_api();
  test_pa_mode();
  test_start_failures();
  test_pairing();
  test_concurrent_pairing();
  test_observer();
  test_dac_cleanup();
  return 0;
}
