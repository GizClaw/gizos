#ifndef H2_GIZCLAW_VTABLE_H
#define H2_GIZCLAW_VTABLE_H
#include "h2/pal/core/h2_pal_errors.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct h2_gizclaw_firmware;

/** Modem identity slots reported in client.identifiers.get. */
#define H2_GIZCLAW_DEVICE_IMEI_MAX 2
#define H2_GIZCLAW_DEVICE_IMEI_NAME_MAX 32

/** One modem IMEI. digits holds exactly 15 ASCII decimal digits plus NUL; the
 * library splits it into TAC (first 8) and serial (last 7). name is an
 * optional NUL-terminated slot label for multi-modem products; an empty
 * string means unnamed. Both are inline buffers so nothing is borrowed from
 * the provider once get_facts returns. */
typedef struct h2_gizclaw_device_imei {
  char digits[16];
  char name[H2_GIZCLAW_DEVICE_IMEI_NAME_MAX + 1];
} h2_gizclaw_device_imei_t;

typedef struct h2_gizclaw_device_facts {
  bool has_battery_percent;
  int64_t battery_percent;
  bool has_charging;
  bool charging;
  bool has_firmware_sha256;
  char firmware_sha256[65];
  /** Cached modem IMEIs, copied out by value. get_facts must not query the
   * modem; report 0 until a cached value exists. */
  size_t imei_count;
  h2_gizclaw_device_imei_t imeis[H2_GIZCLAW_DEVICE_IMEI_MAX];
} h2_gizclaw_device_facts_t;

/** Supplement only capabilities absent from PAL. All arguments are borrowed
 * during the call. get_facts runs on the RPC owner and must not block; Stage
 * methods run on the device task. No callback may destroy or stop the Service.
 * An absent method means unsupported, not successful execution. */
typedef struct h2_gizclaw_vtable {
  h2_pal_result_t (*get_facts)(void *user, h2_gizclaw_device_facts_t *out);
  /** Optional product sound catalog; return a bounded HTTPS Ogg/Opus URL.
   * Called on the device worker. Download/decode/play remain library-owned. */
  h2_pal_result_t (*resolve_sound_url)(void *user, const char *name,
                                       char *out_url, size_t capacity);
  /** Stage backend must reserve an invalid staging area and persist update_id.
   * finish verifies size, SHA-256, board/target and package manifest before
   * publishing Stage. abort invalidates partial staging. */
  h2_pal_result_t (*ota_begin)(void *user,
                               const struct h2_gizclaw_firmware *firmware,
                               const char *update_id);
  h2_pal_result_t (*ota_write)(void *user, const uint8_t *data, size_t length);
  h2_pal_result_t (*ota_finish)(void *user);
  void (*ota_abort)(void *user);
  /** Schedule H2Loader upgrade on the product owner. This is not success:
   * the next firmware must verify its identity and send SUCCEEDED telemetry
   * using the saved update_id. */
  h2_pal_result_t (*ota_activate)(void *user);
  /** Optional non-blocking handoff for client.device.reboot. Called once on
   * the device worker after the RPC response was sent, with the requested
   * delay in milliseconds. The callback must only copy the request and post
   * it to a product-owned execution context (for example a Runtime custom
   * event) and return promptly: it must not sleep, block, or stop or destroy
   * the Service inline. The product applies the delay, runs its orderly
   * shutdown on its own owner and reboots. A returned error is logged as the
   * action result; the library never falls back to the power PAL. When unset
   * the library waits `delay_ms` on the worker and calls the power PAL. */
  h2_pal_result_t (*request_reboot)(void *user, uint32_t delay_ms);
  /** Optional shared-speaker ownership for library playback (audio player
   * and client.device.sound.play). Set both or neither; Service init rejects
   * a lone hook with H2_PAL_ERR_INVALID_ARG. When set, the device
   * worker calls speaker_acquire before opening its PCM track and exactly one
   * speaker_release after the track is closed, on every exit path including
   * failure, stop and cancellation; the library then never calls the audio
   * PAL's start_speaker or stop_speaker itself. Products that reference-count
   * a speaker or PA shared with other audio must set them. Both run on the
   * device worker and must return promptly. When unset, the library calls
   * start_speaker before playback and leaves the speaker on afterwards. */
  h2_pal_result_t (*speaker_acquire)(void *user);
  h2_pal_result_t (*speaker_release)(void *user);
} h2_gizclaw_vtable_t;
#ifdef __cplusplus
}
#endif
#endif
