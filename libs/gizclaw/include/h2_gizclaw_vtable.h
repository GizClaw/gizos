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

/** Longest DeviceSettings.locale the wire message accepts, excluding NUL. */
#define H2_GIZCLAW_DEVICE_LOCALE_MAX 35

/** Default conversation input mode of the device. Mirrors the Workspace input
 * mode so a device default and a Workspace override share one vocabulary. */
typedef enum h2_gizclaw_device_interaction_mode {
  H2_GIZCLAW_DEVICE_INTERACTION_PUSH_TO_TALK = 1,
  H2_GIZCLAW_DEVICE_INTERACTION_REALTIME = 2,
} h2_gizclaw_device_interaction_mode_t;

/** Feedback the device gives on a physical key press. */
typedef enum h2_gizclaw_device_key_feedback {
  H2_GIZCLAW_DEVICE_KEY_FEEDBACK_NONE = 1,
  H2_GIZCLAW_DEVICE_KEY_FEEDBACK_SOUND = 2,
  H2_GIZCLAW_DEVICE_KEY_FEEDBACK_VIBRATE = 3,
  H2_GIZCLAW_DEVICE_KEY_FEEDBACK_SOUND_AND_VIBRATE = 4,
} h2_gizclaw_device_key_feedback_t;

/** How the device alerts the user to an incoming event. */
typedef enum h2_gizclaw_device_alert_mode {
  H2_GIZCLAW_DEVICE_ALERT_SILENT = 1,
  H2_GIZCLAW_DEVICE_ALERT_VIBRATE = 2,
  H2_GIZCLAW_DEVICE_ALERT_RING = 3,
} h2_gizclaw_device_alert_mode_t;

/**
 * Device-owned configuration exchanged by client.device.settings.get/set.
 *
 * Every member is optional in both directions: on a set an absent member leaves
 * that option unchanged, and on a response an absent member means the device
 * does not support that option, which is what lets one message serve products
 * with different hardware. The numeric members mirror the wire types exactly;
 * the library rejects a request, and fails a response, whose present members
 * are out of range: brightness in [0, 100], timeouts >= 0, locale a
 * well-formed BCP 47 tag of at most H2_GIZCLAW_DEVICE_LOCALE_MAX bytes (a
 * 2-8 letter primary subtag then hyphen-separated 1-8 character alphanumeric
 * subtags, so "zh_CN" is rejected), and each enum one of its named values.
 * locale is an inline buffer, so nothing is borrowed from the product.
 */
typedef struct h2_gizclaw_device_settings {
  bool has_cellular_enabled;
  bool cellular_enabled;
  bool has_screen_off_timeout_ms;
  int64_t screen_off_timeout_ms;
  bool has_screen_brightness;
  int64_t screen_brightness;
  bool has_led_brightness;
  int64_t led_brightness;
  bool has_locale;
  char locale[H2_GIZCLAW_DEVICE_LOCALE_MAX + 1];
  bool has_default_interaction_mode;
  h2_gizclaw_device_interaction_mode_t default_interaction_mode;
  bool has_key_feedback;
  h2_gizclaw_device_key_feedback_t key_feedback;
  bool has_alert_mode;
  h2_gizclaw_device_alert_mode_t alert_mode;
  bool has_auto_sleep_timeout_ms;
  int64_t auto_sleep_timeout_ms;
  bool has_nfc_enabled;
  bool nfc_enabled;
} h2_gizclaw_device_settings_t;

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
  /** Optional device configuration for client.device.settings.get/set. Both run
   * on the RPC owner and must return promptly, like get_facts; the library owns
   * the protobuf and the range checks. get_device_settings fills out with the
   * options the product supports and leaves every other member absent.
   * set_device_settings receives only the members the caller sent, already
   * validated, applies them and fills out with the device's full settings after
   * the change, so the caller sees what was accepted. Returning a member out of
   * range fails the RPC instead of sending it. Either hook unset answers
   * UNIMPLEMENTED for its method. */
  h2_pal_result_t (*get_device_settings)(void *user,
                                        h2_gizclaw_device_settings_t *out);
  h2_pal_result_t (*set_device_settings)(
      void *user, const h2_gizclaw_device_settings_t *patch,
      h2_gizclaw_device_settings_t *out);
  /** Optional non-blocking handoff for client.device.factory_reset. Called once
   * on the device worker after the RPC response was sent, like request_reboot:
   * copy the request, post the erase to a product-owned execution context and
   * return promptly. It must not sleep, block, or stop or destroy the Service
   * inline. keep_network asks the product to keep saved Wi-Fi and cellular
   * configuration so the device can reconnect without being re-provisioned.
   * There is no library fallback, because what device-local state means is a
   * product decision; unset answers UNIMPLEMENTED. A product that deletes its
   * own Peer during the reset invalidates every API key of that Peer, including
   * the caller's. */
  h2_pal_result_t (*request_factory_reset)(void *user, bool keep_network);
  /** Optional non-blocking handoff for client.run.workspace.set. Called once on
   * the device worker after the RPC response was sent, with a NUL-terminated
   * name in library storage that is valid only for the call. The library does
   * not switch the Workspace itself: the App owns the Session, its conversation
   * and its confirmed parameters. Copy the name, post a
   * h2_gizclaw_session_select() for it to the App thread and return promptly;
   * kickoff is best expressed as initiative AGENT with agent_initiative_policy
   * ON_RELOAD in that selection's parameter patch. The response only means the
   * request was accepted; the committed Workspace is the one the Server
   * reports. Unset answers UNIMPLEMENTED. */
  h2_pal_result_t (*request_run_workspace_set)(void *user,
                                               const char *workspace_name,
                                               bool kickoff);
} h2_gizclaw_vtable_t;
#ifdef __cplusplus
}
#endif
#endif
