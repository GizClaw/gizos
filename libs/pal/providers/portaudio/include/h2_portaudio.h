#ifndef H2_PORTAUDIO_H
#define H2_PORTAUDIO_H

#include "h2/pal/hal/h2_pal_audio.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/os/h2_pal_queue.h"
#include "h2/pal/os/h2_pal_sync.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_portaudio h2_portaudio_t;

/** PortAudio provider 初始化参数；三个 PAL API 都必须覆盖 provider 生命周期。 */
typedef struct h2_portaudio_config {
  /** 借用的必填 Memory PAL。 */
  const h2_pal_mem_api_t *allocator;
  /** 借用的必填 Queue PAL。 */
  const h2_pal_queue_api_t *queue;
  /** 借用的必填 Sync PAL，并转交给内部 Audio Mixer。 */
  const h2_pal_sync_api_t *sync;
  /** 非零时要求真实输入和输出设备，不允许 synthetic fallback。 */
  int require_real_devices;
} h2_portaudio_config_t;

int h2_portaudio_create(const h2_portaudio_config_t *config,
                        h2_portaudio_t **out_provider);
void h2_portaudio_destroy(h2_portaudio_t *provider);
h2_pal_audio_t *h2_portaudio_audio(h2_portaudio_t *provider);

/** Borrowed interleaved S16 PCM; storage lasts only during the callback.
 * Speaker data follows mixer/volume and successful device writes; timestamp_us
 * estimates its first-sample DAC time from write completion and reported output
 * latency, continuing by sample count until an underrun or a gap over 20 ms.
 * Mic data is real input after AEC, before delivery to the Audio PAL queue;
 * timestamp_us marks read completion minus the frame duration. It is not an
 * ADC hardware timestamp. Both use native steady-clock microseconds.
 */
typedef struct h2_portaudio_capture_frame {
  const int16_t *samples;
  size_t frames;
  uint32_t sample_rate;
  uint8_t channels;
  uint64_t timestamp_us;
} h2_portaudio_capture_frame_t;
typedef void (*h2_portaudio_capture_fn)(
    void *user, const h2_portaudio_capture_frame_t *frame);

typedef struct h2_portaudio_capture_hooks {
  void *user;
  h2_portaudio_capture_fn on_mic;
  h2_portaudio_capture_fn on_speaker;
} h2_portaudio_capture_hooks_t;

/** Copy optional observer hooks; NULL or empty hooks unregister synchronously.
 * One registration per provider; replacement while occupied returns BUSY.
 * Mic and speaker callbacks are serialized with each other. They must return
 * promptly and must not reenter PortAudio; retain data by copying it and move
 * expensive work to the consumer's worker. The provider assigns no purpose to
 * callbacks and does not own user. Unregister waits for in-flight callbacks;
 * call it before releasing user or destroying the provider. Synthetic mic
 * fallback is never reported as real capture. No microphone is started here.
 */
h2_pal_result_t h2_portaudio_set_capture_hooks(
    h2_portaudio_t *provider, const h2_portaudio_capture_hooks_t *hooks);

#ifdef __cplusplus
}
#endif

#endif
