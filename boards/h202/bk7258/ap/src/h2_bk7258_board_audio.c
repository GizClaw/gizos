#include "h2_bk7258_board_private.h"

#include "h2_atomic_static.h"
#include <string.h>

#ifdef H2_H202_AUDIO_HOST_TEST
#include "h2_h202_board_test_sdk.h"
#else
#include "h2_bk_platform_core.h"
#include "components/bk_audio/audio_streams/onboard_dual_dmic_mic_stream.h"
#include "components/bk_audio/audio_streams/onboard_speaker_stream.h"
#include "components/bk_audio/audio_pipeline/rb_port.h"
#include "driver/gpio.h"
#include "gpio_driver.h"
#include "modules/aec.h"
#include "audio_osi_wrapper.h"
#include "os/os.h"
#endif

enum {
  SAMPLES = H2_H202_AUDIO_FRAME_SAMPLES,
  MONO_BYTES = SAMPLES * 2,
  STEREO_BYTES = MONO_BYTES * 2,
  CAPTURE_FRAMES = 4,
  STOP_MS = 1000
};

typedef enum { TASK_NONE, TASK_UNCONFIRMED, TASK_CREATED } element_task_t;

typedef struct {
  int16_t mic[SAMPLES * 2];
  int16_t ref[SAMPLES];
} capture_frame_t;

/* Exactly one product audio owner. MFG joins a prompt before beginning its
 * acoustic operation; no hidden mixer or detached audio worker is needed. */
static struct {
  audio_element_handle_t capture;
  audio_element_handle_t playback;
  audio_port_handle_t microphones;
  audio_port_handle_t reference;
  audio_port_handle_t speaker;
  AECContext* aec[2];
  int16_t* aec_ref[2];
  int16_t* aec_mic[2];
  int16_t* aec_out[2];
  bool capture_started;
  bool playback_started;
  element_task_t capture_task;
  element_task_t playback_task;
  bool track_open;
  beken_mutex_t capture_lock;
  bool capture_lock_initialized;
  bool reference_pending;
  unsigned capture_head;
  unsigned capture_count;
  capture_frame_t frames[CAPTURE_FRAMES];
  capture_frame_t input;
  unsigned volume;
  unsigned factor;
  int16_t output[SAMPLES];
  /* Observer copies: raw packs left, right and the MIC2 reference lane. */
  int16_t observed_raw[SAMPLES * 3];
  int16_t observed_ref[SAMPLES];
  h2_audio_aec_observer_t observer;
  bool observer_set;
  uint64_t sequence;
} s_audio = {.volume = 35, .factor = 1000};

H2_ATOMIC_DEFINE_STATIC(bool, s_capture_fault, false);

static h2_audio_pcm_format_t pcm(unsigned channels) {
  return (h2_audio_pcm_format_t){.sample_rate_hz = H2_H202_AUDIO_RATE_HZ,
                                 .frame_samples_per_channel = SAMPLES,
                                 .channels = (uint8_t)channels,
                                 .sample_format = H2_AUDIO_SAMPLE_S16LE};
}

static h2_audio_frame_t observed(int16_t* data, unsigned channels) {
  return (h2_audio_frame_t){.data = data,
                            .capacity = MONO_BYTES * channels,
                            .bytes = MONO_BYTES * channels,
                            .sample_rate_hz = H2_H202_AUDIO_RATE_HZ,
                            .samples_per_channel = SAMPLES,
                            .channels = (uint8_t)channels,
                            .sample_format = H2_AUDIO_SAMPLE_S16LE};
}

static int run_element(audio_element_handle_t element, element_task_t* task) {
  *task = TASK_UNCONFIRMED;
  if (audio_element_run(element) != 0) {
    /* SDK sets ERROR only on create-thread failure, before returning. A
     * TASK_CREATED_BIT timeout leaves INIT and may have a delayed worker. */
    if (audio_element_get_state(element) == AEL_STATE_ERROR) *task = TASK_NONE;
    return H2_PAL_ERR_IO;
  }
  *task = TASK_CREATED;
  return H2_PAL_OK;
}

static h2_pal_result_t stop_element(audio_element_handle_t* element,
                                    element_task_t* task) {
  if (*element == NULL) return H2_PAL_OK;
  if (*task != TASK_NONE) {
    int stopped = audio_element_stop(*element);
    /* No public SDK thread handle exists. If run timed out before its task
     * announced itself, stop's !task_run result is not proof of absence.
     * Retain ownership until a later stop observes the worker. */
    if (stopped != 0 && *task == TASK_UNCONFIRMED) return H2_PAL_ERR_TIMEOUT;
    *task = TASK_CREATED;
    if (stopped == 0 &&
        audio_element_wait_for_stop_ms(*element, BK_MS_TO_TICKS(STOP_MS)) != 0)
      return H2_PAL_ERR_TIMEOUT;
    /* For a confirmed task, stop can also fail after it already exited.
     * SDK terminate handles !task_run as OK, allowing a timed-out previous
     * termination to be retried after the worker finishes. */
    if (audio_element_terminate_with_ticks(*element, BK_MS_TO_TICKS(STOP_MS)) !=
        0)
      return H2_PAL_ERR_TIMEOUT;
    *task = TASK_NONE;
  }
  /* Custom capture ports belong to this board. The SDK otherwise destroys
   * a PORT_TYPE_CB primary output in element_deinit (but not multi-output). */
  if (audio_element_set_output_port(*element, NULL) != 0) return H2_PAL_ERR_IO;
  if (audio_element_deinit(*element) != 0) return H2_PAL_ERR_IO;
  *element = NULL;
  return H2_PAL_OK;
}

static h2_pal_result_t free_port(audio_port_handle_t* port) {
  if (*port != NULL) {
    if (audio_port_deinit(*port) != 0) return H2_PAL_ERR_IO;
    *port = NULL;
  }
  return H2_PAL_OK;
}

static int stop_mic(void* user) {
  (void)user;
  h2_pal_result_t rc = stop_element(&s_audio.capture, &s_audio.capture_task);
  if (rc != H2_PAL_OK) return rc;
  s_audio.capture_started = false;
  rc = free_port(&s_audio.microphones);
  if (rc == H2_PAL_OK) rc = free_port(&s_audio.reference);
  if (rc != H2_PAL_OK) return rc;
  for (unsigned i = 0; i < 2; ++i) {
    h2_pal_mem_free(h2_bk_platform_psram_allocator(), s_audio.aec[i]);
    s_audio.aec[i] = NULL;
    s_audio.aec_ref[i] = NULL;
    s_audio.aec_mic[i] = NULL;
    s_audio.aec_out[i] = NULL;
  }
  if (s_audio.capture_lock_initialized) {
    if (rtos_deinit_mutex(&s_audio.capture_lock) != 0) return H2_PAL_ERR_IO;
    s_audio.capture_lock_initialized = false;
  }
  s_audio.reference_pending = false;
  s_audio.capture_head = 0;
  s_audio.capture_count = 0;
  s_audio.sequence = 0;
  h2_atomic_bool_store(&s_capture_fault, false, H2_ATOMIC_SEQ_CST);
  return H2_PAL_OK;
}

/* With dual_dmic=1, dual_dmic_sgl_out=0 and ref_mode=1, the SDK process
 * invokes multi-output(ref, timeout=0), then primary output(stereo mic) on
 * the same task. It ignores the ref write result. Reserve one owned slot
 * for ref, publish only on the matching mic callback, and latch any loss.
 * Consumers never see a half frame, even when a full queue rejects ref. */
static int capture_write(audio_port_handle_t port, char* buffer, int length,
                         TickType_t ticks, void* context) {
  (void)ticks;
  (void)context;
  if (rtos_lock_mutex(&s_audio.capture_lock) != 0) {
    h2_atomic_bool_store(&s_capture_fault, true, H2_ATOMIC_SEQ_CST);
    return AEL_IO_FAIL;
  }
  bool reference = port == s_audio.reference;
  bool valid = !h2_atomic_bool_load(&s_capture_fault, H2_ATOMIC_SEQ_CST) &&
               buffer != NULL &&
               length == (reference ? MONO_BYTES : STEREO_BYTES);
  if (reference) {
    valid = valid && !s_audio.reference_pending &&
            s_audio.capture_count < CAPTURE_FRAMES;
    if (valid) {
      unsigned tail =
          (s_audio.capture_head + s_audio.capture_count) % CAPTURE_FRAMES;
      memcpy(s_audio.frames[tail].ref, buffer, MONO_BYTES);
      s_audio.reference_pending = true;
    }
  } else {
    valid = valid && port == s_audio.microphones && s_audio.reference_pending;
    if (valid) {
      unsigned tail =
          (s_audio.capture_head + s_audio.capture_count) % CAPTURE_FRAMES;
      memcpy(s_audio.frames[tail].mic, buffer, STEREO_BYTES);
      s_audio.reference_pending = false;
      ++s_audio.capture_count;
    }
  }
  if (!valid) h2_atomic_bool_store(&s_capture_fault, true, H2_ATOMIC_SEQ_CST);
  if (rtos_unlock_mutex(&s_audio.capture_lock) != 0) {
    h2_atomic_bool_store(&s_capture_fault, true, H2_ATOMIC_SEQ_CST);
    return AEL_IO_FAIL;
  }
  return valid ? length : AEL_IO_FAIL;
}

static audio_port_handle_t capture_port(void) {
  audio_port_cfg_t config = {.write = capture_write};
  audio_port_handle_t port = audio_port_init(&config);
  if (port != NULL && audio_port_set_type(port, PORT_TYPE_CB) != 0) {
    audio_port_deinit(port);
    return NULL;
  }
  return port;
}

static int init_mic(void* user) {
  (void)user;
  if (s_audio.capture_started) return H2_PAL_ERR_BUSY;
  if (s_audio.capture != NULL || s_audio.microphones != NULL ||
      s_audio.reference != NULL || s_audio.aec[0] != NULL ||
      s_audio.aec[1] != NULL || s_audio.capture_lock_initialized)
    return H2_PAL_ERR_INVALID_STATE;
  if (bk_audio_osi_funcs_init() != 0) return H2_PAL_ERR_IO;
  for (unsigned i = 0; i < 2; ++i) {
    /* Two independent AEC states; a shared state would cross-contaminate the
     * microphone results. The ADC reference taps the DAC output ahead of the
     * PA through BK_MIC2P/N. */
    s_audio.aec[i] =
        h2_pal_mem_alloc(h2_bk_platform_psram_allocator(), aec_size(320));
    if (s_audio.aec[i] == NULL) return H2_PAL_ERR_NO_MEMORY;
    memset(s_audio.aec[i], 0, aec_size(320));
    aec_init(s_audio.aec[i], H2_H202_AUDIO_RATE_HZ);
    uint32_t frame_samples = 0;
    aec_ctrl(s_audio.aec[i], AEC_CTRL_CMD_GET_FRAME_SAMPLE,
             (uintptr_t)&frame_samples);
    if (frame_samples != SAMPLES) return H2_PAL_ERR_UNSUPPORTED;
    uintptr_t address = 0;
    aec_ctrl(s_audio.aec[i], AEC_CTRL_CMD_GET_RX_BUF, (uintptr_t)&address);
    s_audio.aec_ref[i] = (int16_t*)(uintptr_t)address;
    aec_ctrl(s_audio.aec[i], AEC_CTRL_CMD_GET_TX_BUF, (uintptr_t)&address);
    s_audio.aec_mic[i] = (int16_t*)(uintptr_t)address;
    aec_ctrl(s_audio.aec[i], AEC_CTRL_CMD_GET_OUT_BUF, (uintptr_t)&address);
    s_audio.aec_out[i] = (int16_t*)(uintptr_t)address;
    if (s_audio.aec_ref[i] == NULL || s_audio.aec_mic[i] == NULL ||
        s_audio.aec_out[i] == NULL)
      return H2_PAL_ERR_IO;
    aec_ctrl(s_audio.aec[i], AEC_CTRL_CMD_SET_FLAGS, 0x1f);
    aec_ctrl(s_audio.aec[i], AEC_CTRL_CMD_SET_MIC_DELAY, 80);
  }
  if (rtos_init_mutex(&s_audio.capture_lock) != 0) return H2_PAL_ERR_NO_MEMORY;
  s_audio.capture_lock_initialized = true;
  s_audio.microphones = capture_port();
  s_audio.reference = capture_port();
  if (s_audio.microphones == NULL || s_audio.reference == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  onboard_dual_dmic_mic_stream_cfg_t config =
      DEFAULT_ONBOARD_DUAL_DMIC_STREAM_CONFIG();
  config.adc_cfg.sample_rate = H2_H202_AUDIO_RATE_HZ;
  config.adc_cfg.chl_num =
      2; /* Differential MIC2 is the schematic DAC reference. */
  config.frame_size = MONO_BYTES;
  config.out_block_size = STEREO_BYTES;
  config.out_block_num = 4;
  config.dual_dmic = 1;
  config.dual_dmic_sgl_out = 0;
  config.ref_mode = 1;
  config.multi_out_port_num = 1;
  config.task_stack = 8192;
  config.task_core = 1;
  config.task_prio = 6;
  s_audio.capture = onboard_dual_dmic_mic_stream_init(&config);
  if (s_audio.capture == NULL) return H2_PAL_ERR_NO_MEMORY;
  if (audio_element_set_output_port(s_audio.capture, s_audio.microphones) !=
          0 ||
      audio_element_set_multi_output_port(s_audio.capture, s_audio.reference,
                                          0) != 0 ||
      audio_element_set_output_timeout(s_audio.capture, BK_MS_TO_TICKS(100)) !=
          0)
    return H2_PAL_ERR_IO;
  if (run_element(s_audio.capture, &s_audio.capture_task) != 0 ||
      audio_element_resume(s_audio.capture, 0, BK_MS_TO_TICKS(STOP_MS)) != 0)
    return H2_PAL_ERR_IO;
  s_audio.capture_started = true;
  return H2_PAL_OK;
}

static int start_mic(void* user) {
  if (s_audio.capture_started) return H2_PAL_ERR_BUSY;
  int rc = init_mic(user);
  if (rc == H2_PAL_OK) return rc;
  int cleanup = stop_mic(user);
  return cleanup == H2_PAL_OK ? rc : cleanup;
}

static int read_mic(void* user, h2_audio_frame_t* out, uint32_t timeout_ms) {
  (void)user;
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  out->bytes = 0;
  if (out->data == NULL || out->capacity < STEREO_BYTES ||
      timeout_ms == UINT32_MAX)
    return H2_PAL_ERR_INVALID_ARG;
  if (!s_audio.capture_started) return H2_PAL_ERR_INVALID_STATE;
  uint64_t before = 0, now = 0;
  if (h2_pal_time_get_monotonic_ms(h2_bk_platform_time_api(), &before) != 0)
    return H2_PAL_ERR_IO;
  for (;;) {
    if (rtos_lock_mutex(&s_audio.capture_lock) != 0) return H2_PAL_ERR_IO;
    bool fault = h2_atomic_bool_load(&s_capture_fault, H2_ATOMIC_SEQ_CST);
    bool ready = !fault && s_audio.capture_count > 0;
    if (ready) {
      s_audio.input = s_audio.frames[s_audio.capture_head];
      s_audio.capture_head = (s_audio.capture_head + 1) % CAPTURE_FRAMES;
      --s_audio.capture_count;
    }
    if (rtos_unlock_mutex(&s_audio.capture_lock) != 0) {
      h2_atomic_bool_store(&s_capture_fault, true, H2_ATOMIC_SEQ_CST);
      return H2_PAL_ERR_IO;
    }
    if (fault) {
      if (s_audio.observer_set)
        s_audio.observer.on_error(s_audio.observer.user, 1, H2_PAL_ERR_IO);
      return H2_PAL_ERR_IO;
    }
    if (ready) break;
    if (h2_pal_time_get_monotonic_ms(h2_bk_platform_time_api(), &now) != 0 ||
        now < before)
      return H2_PAL_ERR_IO;
    if (now - before >= timeout_ms) return H2_PAL_ERR_WOULD_BLOCK;
    rtos_delay_milliseconds(1);
  }
  int16_t* output = out->data;
  if (s_audio.observer_set) {
    for (unsigned i = 0; i < SAMPLES; ++i) {
      s_audio.observed_raw[i * 3] = s_audio.input.mic[i * 2];
      s_audio.observed_raw[i * 3 + 1] = s_audio.input.mic[i * 2 + 1];
      s_audio.observed_raw[i * 3 + 2] = s_audio.input.ref[i];
    }
    memcpy(s_audio.observed_ref, s_audio.input.ref, MONO_BYTES);
  }
  for (unsigned lane = 0; lane < 2; ++lane) {
    memcpy(s_audio.aec_ref[lane], s_audio.input.ref, MONO_BYTES);
    for (unsigned i = 0; i < SAMPLES; ++i)
      s_audio.aec_mic[lane][i] = s_audio.input.mic[i * 2 + lane];
    aec_proc(s_audio.aec[lane], s_audio.aec_ref[lane], s_audio.aec_mic[lane],
             s_audio.aec_out[lane]);
    for (unsigned i = 0; i < SAMPLES; ++i)
      output[i * 2 + lane] = s_audio.aec_out[lane][i];
    if (s_audio.observer_set) {
      /* Each microphone lane has its own AEC state, so each is one frame. */
      h2_audio_frame_t raw = observed(s_audio.observed_raw, 3);
      h2_audio_frame_t reference = observed(s_audio.observed_ref, 1);
      h2_audio_frame_t processed = observed(s_audio.aec_out[lane], 1);
      const h2_audio_aec_frame_t frame = {.sequence = s_audio.sequence++,
                                          .raw = &raw,
                                          .reference_input = &reference,
                                          .processed = &processed,
                                          .microphone_lane = (uint8_t)lane,
                                          .reference_lane = 2,
                                          .microphone_mask = 3};
      s_audio.observer.on_capture(s_audio.observer.user, &frame);
    }
  }
  out->sample_rate_hz = H2_H202_AUDIO_RATE_HZ;
  out->samples_per_channel = SAMPLES;
  out->channels = 2;
  out->sample_format = H2_AUDIO_SAMPLE_S16LE;
  out->bytes = STEREO_BYTES;
  return H2_PAL_OK;
}

static int init_speaker(void* user) {
  (void)user;
  if (s_audio.playback_started) return H2_PAL_OK;
  if (s_audio.playback != NULL || s_audio.speaker != NULL)
    return H2_PAL_ERR_INVALID_STATE;
  if (bk_audio_osi_funcs_init() != 0) return H2_PAL_ERR_IO;
  ringbuf_port_cfg_t port_config = {.ringbuf_size = MONO_BYTES * 4};
  s_audio.speaker = ringbuf_port_init(&port_config);
  if (s_audio.speaker == NULL) return H2_PAL_ERR_NO_MEMORY;
  onboard_speaker_stream_cfg_t config = DEFAULT_ONBOARD_SPEAKER_STREAM_CONFIG();
  config.sample_rate = H2_H202_AUDIO_RATE_HZ;
  config.frame_size = MONO_BYTES;
  config.task_stack = 8192;
  config.task_core = 1;
  config.task_prio = 5;
  config.pa_ctrl_en = true;
  config.pa_ctrl_gpio = H2_H202_PA_ENABLE_GPIO;
  config.pa_on_level = 1;
  config.pa_on_delay = 10;
  config.pa_off_delay = 10;
  s_audio.playback = onboard_speaker_stream_init(&config);
  if (s_audio.playback == NULL) return H2_PAL_ERR_NO_MEMORY;
  if (audio_element_set_input_port(s_audio.playback, s_audio.speaker) != 0 ||
      audio_element_set_input_timeout(s_audio.playback, BK_MS_TO_TICKS(100)) !=
          0)
    return H2_PAL_ERR_IO;
  if (run_element(s_audio.playback, &s_audio.playback_task) != 0 ||
      audio_element_resume(s_audio.playback, 0, BK_MS_TO_TICKS(STOP_MS)) != 0)
    return H2_PAL_ERR_IO;
  s_audio.playback_started = true;
  return H2_PAL_OK;
}

static int stop_speaker(void* user) {
  (void)user;
  if (s_audio.track_open) return H2_PAL_ERR_BUSY;
  int rc = stop_element(&s_audio.playback, &s_audio.playback_task);
  if (rc != H2_PAL_OK) return rc;
  s_audio.playback_started = false;
  return free_port(&s_audio.speaker);
}

static int start_speaker(void* user) {
  int rc = init_speaker(user);
  if (rc == H2_PAL_OK) return rc;
  int cleanup = stop_speaker(user);
  return cleanup == H2_PAL_OK ? rc : cleanup;
}

static int track_write(h2_pal_audio_track_t* track,
                       const h2_audio_frame_t* frame, uint32_t timeout_ms) {
  (void)track;
  if (frame == NULL || frame->data == NULL || frame->channels != 1 ||
      frame->sample_format != H2_AUDIO_SAMPLE_S16LE ||
      frame->sample_rate_hz != H2_H202_AUDIO_RATE_HZ ||
      frame->bytes != MONO_BYTES || frame->capacity < frame->bytes ||
      timeout_ms == UINT32_MAX)
    return H2_PAL_ERR_INVALID_ARG;
  if (!s_audio.playback_started || !s_audio.track_open)
    return H2_PAL_ERR_CLOSED;
  const int16_t* input = frame->data;
  for (unsigned i = 0; i < SAMPLES; ++i) {
    int64_t sample =
        (int64_t)input[i] * s_audio.volume * s_audio.factor / 100000;
    s_audio.output[i] = (int16_t)(sample > INT16_MAX   ? INT16_MAX
                                  : sample < INT16_MIN ? INT16_MIN
                                                       : sample);
  }
  int written = audio_port_write(s_audio.speaker, (char*)s_audio.output,
                                 MONO_BYTES, BK_MS_TO_TICKS(timeout_ms));
  if (written != MONO_BYTES) {
    if (s_audio.observer_set)
      s_audio.observer.on_error(s_audio.observer.user, 0, H2_PAL_ERR_IO);
    return H2_PAL_ERR_IO;
  }
  if (s_audio.observer_set) {
    h2_audio_frame_t accepted = observed(s_audio.output, 1);
    s_audio.observer.on_playback(s_audio.observer.user, &accepted);
  }
  return H2_PAL_OK;
}

static int track_close(h2_pal_audio_track_t* track) {
  (void)track;
  s_audio.track_open = false;
  return H2_PAL_OK;
}

static int track_drain(h2_pal_audio_track_t* track, uint32_t timeout_ms) {
  (void)track;
  if (!s_audio.track_open || timeout_ms == UINT32_MAX)
    return H2_PAL_ERR_INVALID_ARG;
  unsigned elapsed = 0;
  while (audio_port_get_filled_size(s_audio.speaker) > 0) {
    if (elapsed >= timeout_ms) return H2_PAL_ERR_TIMEOUT;
    rtos_delay_milliseconds(1);
    ++elapsed;
  }
  /* Last dequeued frame may still be in the two-frame DAC DMA ring. */
  if (timeout_ms - elapsed < 60) return H2_PAL_ERR_TIMEOUT;
  rtos_delay_milliseconds(60);
  return H2_PAL_OK;
}

static int create_track(void* user, const h2_audio_track_config_t* config,
                        h2_pal_audio_track_t** out) {
  (void)user;
  static h2_pal_audio_track_t track;
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  if (config == NULL || config->format.channels != 1 ||
      config->format.sample_rate_hz != H2_H202_AUDIO_RATE_HZ ||
      config->format.sample_format != H2_AUDIO_SAMPLE_S16LE ||
      config->volume_factor_milli > 1000)
    return H2_PAL_ERR_INVALID_ARG;
  if (!s_audio.playback_started) return H2_PAL_ERR_INVALID_STATE;
  if (s_audio.track_open) return H2_PAL_ERR_BUSY;
  s_audio.factor = config->volume_factor_milli;
  s_audio.track_open = true;
  track = (h2_pal_audio_track_t){.audio = h2_h202_audio_api(),
                                 .write = track_write,
                                 .close = track_close,
                                 .drain = track_drain};
  *out = &track;
  return H2_PAL_OK;
}

static int get_info(void* user, h2_audio_info_t* out) {
  (void)user;
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out = (h2_audio_info_t){.available = 1,
                           .mic_supported = 1,
                           .playback_supported = 1,
                           .mic_format = pcm(2),
                           .playback_format = pcm(1),
                           .mic_queue_frames = 4,
                           .track_queue_frames = 4,
                           .max_tracks = 1,
                           .aec_enabled = 1};
  return H2_PAL_OK;
}

static int set_aec_observer(void* user,
                            const h2_audio_aec_observer_t* observer) {
  (void)user;
  if (s_audio.capture_started || s_audio.playback_started)
    return H2_PAL_ERR_INVALID_STATE;
  if (observer == NULL) {
    memset(&s_audio.observer, 0, sizeof(s_audio.observer));
    s_audio.observer_set = false;
    return H2_PAL_OK;
  }
  s_audio.observer = *observer;
  s_audio.observer_set = true;
  return H2_PAL_OK;
}

static int set_volume(void* user, uint32_t percent) {
  (void)user;
  if (percent > 100) return H2_PAL_ERR_INVALID_ARG;
  s_audio.volume = percent;
  return H2_PAL_OK;
}

static int get_volume(void* user, uint32_t* out) {
  (void)user;
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out = s_audio.volume;
  return H2_PAL_OK;
}

const h2_pal_audio_api_t* h2_h202_audio_api(void) {
  static const h2_pal_audio_vtable_t vtable = {
      .get_info = get_info,
      .start_mic = start_mic,
      .stop_mic = stop_mic,
      .start_speaker = start_speaker,
      .stop_speaker = stop_speaker,
      .mic_read = read_mic,
      .create_track = create_track,
      .get_speaker_volume_percent = get_volume,
      .set_speaker_volume_percent = set_volume,
      .set_aec_observer = set_aec_observer};
  static const h2_pal_audio_api_t api = {.vtable = &vtable};
  return &api;
}

h2_pal_result_t h2_h202_audio_deinit(void) {
  int rc = stop_mic(NULL);
  if (rc == H2_PAL_OK) rc = stop_speaker(NULL);
  return rc;
}
