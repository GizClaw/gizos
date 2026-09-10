#ifndef H2_WEB_PLATFORM_H
#define H2_WEB_PLATFORM_H

#include "h2_libco.h"
#include "h2_pal.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque reusable Browser/WebAssembly PAL provider state. */
typedef struct h2_web_platform h2_web_platform_t;

typedef struct h2_web_platform_config {
  /** Positive logical Canvas width in pixels. */
  int32_t display_width;
  /** Positive logical Canvas height in pixels. */
  int32_t display_height;
} h2_web_platform_config_t;

/**
 * Create one single-threaded Browser provider and cooperative executor.
 *
 * @param config Borrowed dimensions copied before this call returns.
 * @return An owned platform, or NULL for invalid dimensions/allocation failure.
 */
h2_web_platform_t *
h2_web_platform_create(const h2_web_platform_config_t *config);

/**
 * Destroy an idle provider after target-owned Runtime/tasks are released.
 *
 * A busy executor or in-flight asynchronous WebRTC call is left intact; retry
 * after those calls return. NULL is accepted. All borrowed accessor
 * results become invalid when destruction succeeds.
 */
void h2_web_platform_destroy(h2_web_platform_t *platform);

/**
 * Run one non-reentrant bounded scheduler turn on the browser root.
 *
 * @param platform Borrowed live platform.
 * @param work_budget Maximum ready tasks resumed during this turn.
 * @param out_resumed Optional count, cleared before validation.
 * @return OK or INVALID_STATE for a dead, reentrant, or shutting-down call.
 */
h2_pal_result_t h2_web_platform_pump(h2_web_platform_t *platform,
                                     size_t work_budget,
                                     size_t *out_resumed);

/** Schedule an asynchronous bounded pump on the browser event loop. */
void h2_web_platform_schedule(h2_web_platform_t *platform);

/** Request cooperative cancellation for one task owned by this platform. */
h2_pal_result_t h2_web_platform_task_cancel(h2_web_platform_t *platform,
                                            h2_pal_task_t *task);

/** Process-lifetime stateless Memory PAL provider. */
const h2_pal_mem_api_t *h2_web_platform_mem_api(void);
/** Process-lifetime stateless console Log PAL provider. */
const h2_pal_log_api_t *h2_web_platform_log_api(void);
/** Borrow real APIs owned by @p platform until successful platform destroy. */
const h2_pal_time_api_t *
h2_web_platform_time_api(h2_web_platform_t *platform);
const h2_pal_timer_api_t *
h2_web_platform_timer_api(h2_web_platform_t *platform);
const h2_pal_task_api_t *
h2_web_platform_task_api(h2_web_platform_t *platform);
const h2_pal_queue_api_t *
h2_web_platform_queue_api(h2_web_platform_t *platform);
const h2_pal_sync_api_t *
h2_web_platform_sync_api(h2_web_platform_t *platform);
const h2_pal_pref_api_t *
h2_web_platform_pref_api(h2_web_platform_t *platform);
/** Browser Fetch HTTP provider. Requests remain subject to browser CORS. */
const h2_pal_http_api_t *
h2_web_platform_http_api(h2_web_platform_t *platform);
/** wolfCrypt provider seeded from browser cryptographic randomness. */
const h2_pal_crypto_api_t *
h2_web_platform_crypto_api(h2_web_platform_t *platform);
const h2_pal_display_api_t *
h2_web_platform_display_api(h2_web_platform_t *platform);
/**
 * Browser Web Audio playback and getUserMedia/AudioWorklet microphone provider.
 *
 * Microphone capture produces 16 kHz mono S16LE, 320 samples per frame, with
 * eight reusable buffers and drop-newest overflow. mic_read requires at least
 * 640 bytes of caller storage; success fills the frame format and byte count.
 * Zero timeout returns WOULD_BLOCK when empty; finite waits return TIMEOUT.
 * Capture requests echoCancellation=true and logs the actual track setting.
 * This is a preference: unconfirmed AEC warns but does not reject capture.
 * Only one read may be pending (another returns BUSY). Task callers yield
 * cooperatively; root callers require Asyncify.
 *
 * Start requires a user gesture, secure context and browser permission; it waits at most
 * 30 seconds. Missing APIs return UNSUPPORTED, denied/unavailable devices return
 * UNAVAILABLE, and browser processing failures return IO. A repeated start
 * returns INVALID_STATE. Stop is idempotent, clears queued PCM and makes pending
 * start/read calls return CLOSED. Late permission results are stopped rather
 * than attached. Device removal returns CLOSED. Stop/cancel outstanding calls
 * and let them return before destroying the platform.
 *
 * While started, Module.h2WebMicrophoneStreams.get(platform_address) exposes a
 * borrowed MediaStream for caller-owned WebRTC tracks. Unset those tracks before
 * stopping capture; PAL stop owns and stops this stream's native tracks.
 * Hosting CSP must permit the microphone worklet's generated blob module.
 */
const h2_pal_audio_api_t *
h2_web_platform_audio_api(h2_web_platform_t *platform);
/** Browser WebCodecs H.264 Annex-B decoder with allocator-backed RGB565 output. */
const h2_pal_video_decoder_api_t *
h2_web_platform_video_decoder_api(h2_web_platform_t *platform);
/** Browser WebCodecs AAC-LC decoder with allocator-backed S16LE output. */
const h2_pal_audio_decoder_api_t *
h2_web_platform_audio_decoder_api(h2_web_platform_t *platform);
const h2_pal_touch_api_t *
h2_web_platform_touch_api(h2_web_platform_t *platform);
const h2_pal_serial_host_api_t *
h2_web_platform_serial_host_api(h2_web_platform_t *platform);
/** Browser RTCPeerConnection/DataChannel provider, called on the JS thread.
 * Media is caller-owned: register {stream: MediaStream, audio:
 * HTMLMediaElement} in Module.h2WebRtcTracks (a Map keyed by a nonzero wasm32
 * integer token), then pass a caller-constructed h2_pal_webrtc_track_t with
 * native_handle equal to that token. Either stream or audio may be omitted, but
 * not both. Keep the objects, registry entry and C Track alive until unset
 * succeeds or peer closes. No automatic getUserMedia, Audio construction, or
 * MediaStreamTrack.stop. unset waits for replaceTrack(null) and clears remote
 * playback; it is valid after an offer. A token may be bound to only one peer
 * at a time. poll only consumes events; the browser drives transport and media.
 * A waiting poll wakes on an event, peer close, or timeout (TIMEOUT);
 * concurrent poll on the same peer returns BUSY. Zero timeout returns
 * WOULD_BLOCK when empty. The receive FIFO holds at most 256 events / 4 MiB of
 * payload and labels. Overflow/allocation failure preserves its accepted
 * prefix, then reports one ERROR event and a sticky error return; close and
 * recreate the peer to recover. Send pressure returns WOULD_BLOCK and
 * bufferedamountlow produces WRITABLE; a message larger than 1 MiB or the
 * negotiated limit returns NO_SPACE.
 */
const h2_pal_webrtc_api_t *
h2_web_platform_webrtc_api(h2_web_platform_t *platform);

/**
 * Start the Web Serial chooser; call only from a direct user gesture.
 *
 * Returns BUSY while another chooser is pending and UNSUPPORTED is reported
 * through the terminal authorization result when Web Serial is unavailable.
 */
h2_pal_result_t
h2_web_platform_serial_request_port(h2_web_platform_t *platform);

/**
 * Read the retained terminal chooser result or WOULD_BLOCK while pending.
 *
 * On success, copies one process-local opaque port ID into caller storage. The
 * terminal result remains readable until the next request.
 */
h2_pal_result_t h2_web_platform_serial_authorization(
    h2_web_platform_t *platform, char *out_port_id, size_t out_size);

/**
 * Revoke the browser authorization of one opaque port ID via
 * SerialPort.forget(). Returns INVALID_ARG for an unknown ID format and BUSY
 * while another forget is pending; the terminal result is read through
 * h2_web_platform_serial_forget_result(): NOT_FOUND when the port is not
 * registered, UNSUPPORTED when the browser lacks forget(), IO on rejection.
 */
h2_pal_result_t h2_web_platform_serial_forget_port(
    h2_web_platform_t *platform, const char *port_id);

/** Read the retained terminal forget result or WOULD_BLOCK while pending. */
h2_pal_result_t h2_web_platform_serial_forget_result(
    h2_web_platform_t *platform);

/**
 * Synchronously reject new serial work and terminate pending PAL operations.
 *
 * This is the page-lifecycle cancellation boundary. It invalidates late
 * Promise completions and makes blocked PAL calls runnable with CLOSED; the
 * owner must keep pumping until its tasks unwind, then destroy the platform.
 * Returns UNSUPPORTED when an active Web Serial session existed because the
 * browser exposes reader/writer cancellation and port close only as Promises;
 * this call initiates that best-effort browser cleanup but cannot claim it
 * completed before a page-lifecycle callback returns.
 */
h2_pal_result_t
h2_web_platform_serial_shutdown(h2_web_platform_t *platform);

/** Legacy pointer snapshot used by the existing Tap Reset Web adapter. */
h2_pal_result_t h2_web_platform_read_pointer(void *user, int32_t *out_x,
                                             int32_t *out_y,
                                             int *out_pressed);
void h2_web_platform_install_pointer(h2_web_platform_t *platform);

#ifdef __cplusplus
}
#endif

#endif
