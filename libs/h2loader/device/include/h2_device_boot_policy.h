#ifndef H2_DEVICE_BOOT_POLICY_H
#define H2_DEVICE_BOOT_POLICY_H

#include "h2_loader_pal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* These keys are the existing preference wire contract, retained for upgrades.
 */
#define H2_JIELI_TRIAL_ATTEMPT_KEY "jieli_trial_attempt"
#define H2_JIELI_TRIAL_CHECKSUM_KEY "jieli_trial_checksum"
#define H2_JIELI_TRIAL_RESET_REASON_KEY "jieli_trial_reset_reason"

typedef enum h2_device_boot_policy_flag {
  H2_DEVICE_BOOT_TRACK_TRIAL = 1u << 0,
  H2_DEVICE_BOOT_PERSIST_RECOVERY = 1u << 1,
  H2_DEVICE_BOOT_STORED_SHADOW = 1u << 2,
  H2_DEVICE_BOOT_PREPARE_SELECTION = 1u << 3,
  H2_DEVICE_BOOT_PREPARE_APP_RESTART = 1u << 4,
  H2_DEVICE_BOOT_FACTORY_IDENTITY = 1u << 5,
} h2_device_boot_policy_flag_t;

typedef enum h2_device_identity_policy {
  /** Hash exactly the public STORED length; unknown length is unsupported. */
  H2_DEVICE_IDENTITY_STORED = 0,
  /** H2 package policy for fixed windows padded to their complete capacity. */
  H2_DEVICE_IDENTITY_PADDED_WINDOW = 1,
  /** An installed package identity remains authoritative when native bytes are
   * no longer the package's representation. Validates the H2 metadata. */
  H2_DEVICE_IDENTITY_COMMITTED = 2,
  /** Hash the public native executable representation (for example, an ESP
   * image whose packaged padding length is unavailable after reset). */
  H2_DEVICE_IDENTITY_NATIVE = 3,
} h2_device_identity_policy_t;

typedef struct h2_device_boot_policy_config {
  const char* app_entry_path;
  const h2_pal_fs_api_t* fs;
  const h2_pal_pref_api_t* pref;
  const h2_pal_mem_api_t* allocator;
  const h2_pal_image_api_t* image;
  const h2_pal_power_api_t* power;
  const h2_pal_time_api_t* time;
  uint32_t loader_partition_id;
  uint32_t app_partition_id;
  uint32_t digest_yield_ms;
  int digest_feed_watchdog;
  uint32_t flags;
  h2_device_identity_policy_t loader_identity;
  h2_device_identity_policy_t app_identity;
} h2_device_boot_policy_config_t;

/** Caller-owned H2 policy over public Image/Power. Serialize whole operations
 * (metadata, SHA, pending evidence, selection, confirmation), not just PAL
 * calls. begin never public-aborts a previously activated candidate: selecting
 * its confirmed running slot is the publication barrier before canonical erase.
 */
typedef struct h2_device_boot_policy {
  h2_device_boot_policy_config_t config;
  h2_loader_pal_t pal;
  h2_pal_image_api_t image;
  h2_pal_power_api_t power;
  h2_bundle_app_writer_t legacy_app_writer;
  uint32_t running_partition_id;
  uint32_t writing_partition_id;
  uint32_t finished_partition_id;
  int candidate_activated;
  int partition_2_shadow_reusable;
  int app_trial_rolled_back;
  int loader_candidate_confirmed;
} h2_device_boot_policy_t;

int h2_device_boot_policy_init(h2_device_boot_policy_t* state,
                               const h2_device_boot_policy_config_t* config);
int h2_device_boot_policy_confirm(void* user);
int h2_device_boot_policy_confirm_app(void* user);
int h2_device_boot_policy_current_identity(h2_device_boot_policy_t* state,
                                           h2_loader_image_role_t role,
                                           const char* board,
                                           const char* target,
                                           const char* version,
                                           h2_loader_image_identity_t* out);
const h2_bundle_app_writer_t* h2_device_boot_policy_legacy_app_writer(
    h2_device_boot_policy_t* state);
int h2_device_boot_policy_clear_trial(const h2_pal_pref_api_t* pref);

#ifdef __cplusplus
}
#endif
#endif
