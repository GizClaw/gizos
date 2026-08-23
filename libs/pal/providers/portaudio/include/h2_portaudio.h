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

#ifdef __cplusplus
}
#endif

#endif
