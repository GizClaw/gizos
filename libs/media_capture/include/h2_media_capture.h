#ifndef H2_MEDIA_CAPTURE_H
#define H2_MEDIA_CAPTURE_H

#include "h2/pal/os/h2_pal_time.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Borrowed, already composited display output; RGB565 pixels are host-endian.
 * brightness is the presentation multiplier (0..255), not window opacity.
 * timestamp_us is the presentation time in the injected monotonic clock.
 * The source display is opaque: alpha blending has already happened in LVGL.
 */
typedef struct h2_media_capture_video {
  const uint16_t *pixels;
  uint32_t width;
  uint32_t height;
  size_t stride_bytes;
  uint8_t brightness;
  uint64_t timestamp_us;
} h2_media_capture_video_t;

/** Borrowed interleaved signed PCM after mixer/volume and successful device
 * write. timestamp_us estimates the first sample's DAC presentation time in
 * the same monotonic clock as video, including provider output latency.
 */
typedef struct h2_media_capture_audio {
  const int16_t *samples;
  size_t frames;
  uint32_t sample_rate;
  uint8_t channels;
  uint64_t timestamp_us;
} h2_media_capture_audio_t;

typedef struct h2_media_capture_vtable {
  void (*video)(void *user, const h2_media_capture_video_t *frame);
  void (*audio)(void *user, const h2_media_capture_audio_t *frame);
  /** Optional source error notification (for example an unavailable clock). */
  void (*error)(void *user, h2_pal_result_t result);
} h2_media_capture_vtable_t;

/** Platform-neutral capture sink. The sink, vtable and time API must outlive
 * registration. Buffers are valid only during the callback. Callbacks may run
 * concurrently from display and speaker tasks; they must copy into bounded
 * storage, must not reenter a source provider, and must not encode or do I/O.
 * Sources unregister under their callback lock, so unregister returning means
 * no callback is using the borrowed sink. A source accepts at most one sink.
 */
typedef struct h2_media_capture_api {
  void *user;
  const h2_media_capture_vtable_t *vtable;
  const h2_pal_time_api_t *time;
} h2_media_capture_api_t;

#ifdef __cplusplus
}
#endif
#endif
