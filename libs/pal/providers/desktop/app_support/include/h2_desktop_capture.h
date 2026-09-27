#ifndef H2_DESKTOP_CAPTURE_H
#define H2_DESKTOP_CAPTURE_H

#include "h2_portaudio.h"
#include "h2_sdl3.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Optional Desktop observers. This is callback configuration, not a PAL
 * capability or dispatch interface. NULL callbacks are disabled. All payloads
 * are borrowed for the call; copy to retain them. Display and audio callbacks
 * may run concurrently, so user storage must support that concurrency.
 * Callbacks must return promptly and must not reenter source providers or
 * capture configuration; delegate expensive work to the consumer's worker.
 * All timestamps are native steady-clock microseconds. Source frame comments
 * specify speaker latency and mic timing/processing semantics.
 */
typedef struct h2_desktop_capture_hooks {
  void *user;
  h2_sdl3_frame_capture_fn on_display;
  h2_portaudio_capture_fn on_mic;
  h2_portaudio_capture_fn on_speaker;
} h2_desktop_capture_hooks_t;

typedef struct h2_desktop_capture_config {
  /** Borrowed; required only when on_display is configured. */
  h2_sdl3_t *display;
  /** Borrowed; required only when an audio callback is configured. */
  h2_portaudio_t *audio;
  /** Copied at create; user remains borrowed until destroy returns. */
  h2_desktop_capture_hooks_t hooks;
} h2_desktop_capture_config_t;

typedef struct h2_desktop_capture h2_desktop_capture_t;

/** Register optional hooks on existing Desktop providers; performs no media
 * encoding, storage, device startup or application policy. NULL config or all
 * NULL callbacks is a successful no-op with *out_capture=NULL. On error the
 * output is NULL and registrations made by this call are undone. Occupied
 * sources return BUSY without replacing another consumer. Call from a control
 * task. Providers and user must outlive the returned registration.
 */
h2_pal_result_t
h2_desktop_capture_create(const h2_desktop_capture_config_t *config,
                          h2_desktop_capture_t **out_capture);

/** Synchronously unregister owned hooks, waiting for in-flight callbacks before
 * releasing the registration. NULL allowed. Call before freeing user or source
 * providers, never from a callback. Do not independently reconfigure a source
 * while this registration owns it. Destroy is serialized by the caller.
 */
void h2_desktop_capture_destroy(h2_desktop_capture_t *capture);

#ifdef __cplusplus
}
#endif
#endif
