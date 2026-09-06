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
typedef struct h2_gizclaw_device_facts {
  bool has_battery_percent;
  int64_t battery_percent;
  bool has_charging;
  bool charging;
  bool has_firmware_sha256;
  char firmware_sha256[65];
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
} h2_gizclaw_vtable_t;
#ifdef __cplusplus
}
#endif
#endif
