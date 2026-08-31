#ifndef H2_DESKTOP_PLATFORM_AUDIO_TEST_H
#define H2_DESKTOP_PLATFORM_AUDIO_TEST_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_portaudio_output_test_ops {
  void *user;
  int (*open)(void *user, void **out_stream);
  int (*start)(void *user, void *stream);
  long (*write_available)(void *user, void *stream);
  int (*write)(void *user, void *stream, const void *samples,
               unsigned long frames);
  int (*stop)(void *user, void *stream);
  int (*abort)(void *user, void *stream);
  int (*close)(void *user, void *stream);
  const char *(*error_text)(void *user, int error);
} h2_portaudio_output_test_ops_t;

typedef struct h2_portaudio_device_ops {
  void *user;
  int (*default_output_device)(void *user);
  int (*default_high_output_latency)(void *user, int device,
                                     double *out_latency);
  int (*open_output)(void *user, int device, double suggested_latency,
                     void **out_stream);
} h2_portaudio_device_ops_t;

typedef struct h2_portaudio_echo_test_ops {
  void *user;
  void (*reset)(void *user);
  void (*before_enqueue)(void *user);
  void (*enqueue_result)(void *user, int queued);
  void (*playback)(void *user, const int16_t *reference);
  void (*capture)(void *user, const int16_t *microphone, int16_t *cleaned);
} h2_portaudio_echo_test_ops_t;

int h2_portaudio_set_output_test_ops(
    h2_portaudio_t *provider,
    const h2_portaudio_output_test_ops_t *ops);

int h2_portaudio_output_underflow_error_for_test(void);

int h2_portaudio_device_unavailable_error_for_test(void);

int h2_portaudio_internal_error_for_test(void);

int h2_portaudio_no_device_for_test(void);

int h2_portaudio_open_default_output_for_test(
    const h2_portaudio_device_ops_t *ops, void **out_stream);

uint64_t h2_portaudio_output_underflow_count_for_test(h2_portaudio_t *provider);

int h2_portaudio_set_require_real_devices_for_test(h2_portaudio_t *provider,
                                                   int require_real_devices);

int h2_portaudio_set_echo_test_ops(h2_portaudio_t *provider,
                                   const h2_portaudio_echo_test_ops_t *ops);

#ifdef __cplusplus
}
#endif

#endif
