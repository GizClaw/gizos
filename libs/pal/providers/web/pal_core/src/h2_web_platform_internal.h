#ifndef H2_WEB_PLATFORM_INTERNAL_H
#define H2_WEB_PLATFORM_INTERNAL_H

#include "h2_web_platform.h"

#include <stdbool.h>

#define H2_WEB_TOUCH_EVENT_CAPACITY 32u

struct h2_pal_webrtc_track {
  h2_web_platform_t *owner;
  h2_pal_webrtc_peer_t *bound_peer;
};

struct h2_web_platform {
  int32_t width;
  int32_t height;
  uint32_t *rgba;
  int32_t pointer_x;
  int32_t pointer_y;
  int pointer_pressed;
  h2_pal_touch_event_t touch_events[H2_WEB_TOUCH_EVENT_CAPACITY];
  size_t touch_head;
  size_t touch_count;
  h2_libco_t *executor;
  h2_pal_time_api_t time_source_api;
  h2_pal_timer_t *timers;
  h2_pal_timer_api_t timer_api;
  h2_pal_pref_api_t pref_api;
  h2_pal_http_api_t http_api;
  h2_pal_audio_api_t audio_api;
  h2_pal_audio_decoder_api_t audio_decoder_api;
  h2_pal_video_decoder_api_t video_decoder_api;
  h2_pal_display_api_t display_api;
  h2_pal_touch_api_t touch_api;
  h2_pal_serial_host_api_t serial_api;
  h2_pal_webrtc_api_t webrtc_api;
  h2_pal_webrtc_track_t webrtc_audio_track;
  void *serial_state;
  h2_pal_webrtc_peer_t *webrtc_peers;
  bool pointer_installed;
  bool crypto_ready;
  h2_pal_time_wall_source_t wall_time_source;
  bool touch_opened;
  bool speaker_started;
  bool speaker_stopped;
  uint32_t speaker_volume_percent;
  bool pumping;
  bool shutting_down;
  bool pump_scheduled;
  uint64_t pump_deadline_ms;
  int64_t wall_time_offset_ms;
};

void h2_web_platform_display_init(h2_web_platform_t *platform);
void h2_web_platform_display_deinit(h2_web_platform_t *platform);
void h2_web_platform_audio_init(h2_web_platform_t *platform);
void h2_web_platform_audio_deinit(h2_web_platform_t *platform);
void h2_web_platform_audio_decoder_init(h2_web_platform_t *platform);
void h2_web_platform_video_decoder_init(h2_web_platform_t *platform);
int h2_web_platform_crypto_init(h2_web_platform_t *platform);
void h2_web_platform_crypto_deinit(h2_web_platform_t *platform);
void h2_web_platform_http_init(h2_web_platform_t *platform);
void h2_web_platform_timer_init(h2_web_platform_t *platform);
void h2_web_platform_timer_deinit(h2_web_platform_t *platform);
void h2_web_platform_timer_dispatch(h2_web_platform_t *platform);
void h2_web_platform_pref_init(h2_web_platform_t *platform);
h2_pal_result_t h2_web_platform_serial_init(h2_web_platform_t *platform);
void h2_web_platform_serial_deinit(h2_web_platform_t *platform);
h2_libco_result_t h2_web_platform_serial_poll(h2_web_platform_t *platform,
                                               h2_libco_t *executor);
void h2_web_platform_request_pump(h2_web_platform_t *platform,
                                  uint64_t deadline_ms);
void h2_web_platform_webrtc_init(h2_web_platform_t *platform);
void h2_web_platform_webrtc_deinit(h2_web_platform_t *platform);

#endif
