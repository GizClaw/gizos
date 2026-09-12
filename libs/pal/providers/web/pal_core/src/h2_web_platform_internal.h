#ifndef H2_WEB_PLATFORM_INTERNAL_H
#define H2_WEB_PLATFORM_INTERNAL_H

#include "h2_web_platform.h"

#include <stdbool.h>

#define H2_WEB_TOUCH_EVENT_CAPACITY 32u
#define H2_WEB_NETIF_NAME "browser"
#define H2_WEB_SYSTEM_EVENT_SUBSCRIPTION_MAX 64u

#define H2_WEB_ASYNC_WAIT_FOREVER UINT32_MAX

typedef struct h2_web_audio_track h2_web_audio_track_t;

/* A finished WebRTC media task waiting for the pump to join it. */
typedef struct h2_web_webrtc_zombie {
  struct h2_web_webrtc_zombie *next;
  h2_pal_task_t *task;
} h2_web_webrtc_zombie_t;

/*
 * One pending browser Promise awaited by C. Register with begin before
 * handing id to JS, wait (repeatable), then end. JS completes it with
 * Module._h2_web_async_complete(platform, id, result); ended ids are ignored.
 */
typedef struct h2_web_async {
  struct h2_web_async *next;
  uint32_t id;
  int result;
  bool done;
  bool woken;
} h2_web_async_t;

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
  h2_pal_netif_api_t netif_api;
  h2_pal_system_event_api_t system_event_api;
  h2_pal_system_event_subscription_t *system_event_subscriptions;
  size_t system_event_subscription_count;
  unsigned system_event_users;
  h2_web_async_t *async_ops;
  uint32_t async_next_id;
  unsigned async_waiters;
  bool async_wake_pending;
  unsigned open_filesystems;
  uint32_t http_next_id;
  unsigned http_requests;
  bool netif_supported;
  bool netif_online;
  bool netif_dirty;
  void *serial_state;
  h2_pal_webrtc_peer_t *webrtc_peers;
  h2_web_webrtc_zombie_t *webrtc_zombies;
  bool pointer_installed;
  bool crypto_ready;
  bool touch_opened;
  uint64_t mic_generation;
  unsigned mic_calls;
  bool mic_starting;
  bool mic_reading;
  h2_web_audio_track_t *audio_tracks;
  bool speaker_started;
  bool speaker_stopped;
  uint32_t speaker_volume_percent;
  bool pumping;
  bool shutting_down;
  bool pump_scheduled;
  uint64_t pump_deadline_ms;
};

void h2_web_platform_display_init(h2_web_platform_t *platform);
void h2_web_platform_display_deinit(h2_web_platform_t *platform);
int h2_web_platform_mic_supported(void);
int h2_web_platform_mic_start(void *user);
int h2_web_platform_mic_stop(void *user);
int h2_web_platform_mic_read(void *user, h2_audio_frame_t *frame, uint32_t timeout_ms);
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
void h2_web_async_begin(h2_web_platform_t *platform, h2_web_async_t *op);
/**
 * Wait until op completes, timeout_ms passes (TIMEOUT) or the calling task is
 * cancelled (CLOSED). Tasks yield to the executor; the root uses Asyncify.
 * OK means op->result holds the completion result.
 */
h2_pal_result_t h2_web_async_wait(h2_web_platform_t *platform,
                                  h2_web_async_t *op, uint32_t timeout_ms);
void h2_web_async_end(h2_web_platform_t *platform, h2_web_async_t *op);
/** Wait forever for op when started is OK, then end it; returns the result. */
int h2_web_async_finish(h2_web_platform_t *platform, h2_web_async_t *op,
                         int started);
/** Complete a registered op from C, e.g. from a browser event callback. */
void h2_web_async_signal(h2_web_platform_t *platform, h2_web_async_t *op,
                         int result);
/** Sleep: tasks yield through libco, the root suspends through Asyncify. */
h2_pal_result_t h2_web_platform_sleep_ms(h2_web_platform_t *platform,
                                         uint32_t duration_ms);
void h2_web_platform_async_poll(h2_web_platform_t *platform,
                                h2_libco_t *executor);
void h2_web_platform_netif_init(h2_web_platform_t *platform);
void h2_web_platform_netif_deinit(h2_web_platform_t *platform);
void h2_web_platform_netif_poll(h2_web_platform_t *platform);
void h2_web_platform_webrtc_init(h2_web_platform_t *platform);
void h2_web_platform_webrtc_deinit(h2_web_platform_t *platform);
bool h2_web_platform_webrtc_busy(h2_web_platform_t *platform);
/** Join finished media tasks; call from the root outside a scheduler turn. */
void h2_web_platform_webrtc_reap(h2_web_platform_t *platform);

#endif
