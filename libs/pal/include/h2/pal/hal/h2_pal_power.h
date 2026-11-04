#ifndef H2_PAL_POWER_H
#define H2_PAL_POWER_H

#include "h2/pal/core/h2_pal_errors.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_PAL_POWER_BOOT_PARTITION_NAME_MAX 32u

typedef enum h2_pal_power_capability {
  H2_PAL_POWER_CAPABILITY_NONE = 0,
  H2_PAL_POWER_CAPABILITY_HOLD = 1u << 0,
  H2_PAL_POWER_CAPABILITY_SHUTDOWN = 1u << 1,
  H2_PAL_POWER_CAPABILITY_REBOOT = 1u << 2,
  H2_PAL_POWER_CAPABILITY_BOOT_PARTITIONS = 1u << 3,
  H2_PAL_POWER_CAPABILITY_SET_NEXT_BOOT_PARTITION = 1u << 4,
  H2_PAL_POWER_CAPABILITY_SLEEP = 1u << 5,
  H2_PAL_POWER_CAPABILITY_DEEP_SLEEP = 1u << 6,
  H2_PAL_POWER_CAPABILITY_BOOT_SOURCE = 1u << 7,
  H2_PAL_POWER_CAPABILITY_RESET_REASON = 1u << 8,
  H2_PAL_POWER_CAPABILITY_DEEP_SLEEP_WAKE_TIMER = 1u << 9,
  H2_PAL_POWER_CAPABILITY_IMAGE_STATE = 1u << 10,
  H2_PAL_POWER_CAPABILITY_PREPARE_BOOT = 1u << 11,
  H2_PAL_POWER_CAPABILITY_CONFIRM_IMAGE = 1u << 12,
  H2_PAL_POWER_CAPABILITY_WATCHDOG_FEED = 1u << 13,
} h2_pal_power_capability_t;

typedef struct h2_pal_power_capabilities {
  uint32_t flags;
} h2_pal_power_capabilities_t;

typedef enum h2_pal_power_state {
  H2_PAL_POWER_STATE_UNKNOWN = 0,
  H2_PAL_POWER_STATE_RUNNING,
  H2_PAL_POWER_STATE_PREPARING_SHUTDOWN,
  H2_PAL_POWER_STATE_SHUTTING_DOWN,
  H2_PAL_POWER_STATE_REBOOTING,
  H2_PAL_POWER_STATE_SLEEPING,
  H2_PAL_POWER_STATE_DEEP_SLEEPING,
  H2_PAL_POWER_STATE_OFF,
} h2_pal_power_state_t;

typedef enum h2_pal_power_boot_partition_flag {
  H2_PAL_POWER_BOOT_PARTITION_FLAG_NONE = 0,
  H2_PAL_POWER_BOOT_PARTITION_FLAG_BOOTABLE = 1u << 0,
  H2_PAL_POWER_BOOT_PARTITION_FLAG_RUNNING = 1u << 1,
  H2_PAL_POWER_BOOT_PARTITION_FLAG_NEXT = 1u << 2,
  H2_PAL_POWER_BOOT_PARTITION_FLAG_FACTORY = 1u << 3,
  H2_PAL_POWER_BOOT_PARTITION_FLAG_APP = 1u << 4,
  H2_PAL_POWER_BOOT_PARTITION_FLAG_TEST = 1u << 5,
  H2_PAL_POWER_BOOT_PARTITION_FLAG_RECOVERY = 1u << 6,
} h2_pal_power_boot_partition_flag_t;

typedef enum h2_pal_power_boot_source {
  H2_PAL_POWER_BOOT_SOURCE_UNKNOWN = 0,
  H2_PAL_POWER_BOOT_SOURCE_COLD_BOOT,
  H2_PAL_POWER_BOOT_SOURCE_GPIO_IRQ,
  H2_PAL_POWER_BOOT_SOURCE_TIMER,
} h2_pal_power_boot_source_t;

typedef enum h2_pal_power_reset_reason {
  H2_PAL_POWER_RESET_REASON_UNKNOWN = 0,
  H2_PAL_POWER_RESET_REASON_POWER_ON,
  H2_PAL_POWER_RESET_REASON_SOFTWARE,
  H2_PAL_POWER_RESET_REASON_WATCHDOG,
  H2_PAL_POWER_RESET_REASON_BROWNOUT,
  H2_PAL_POWER_RESET_REASON_RESET_PIN,
  H2_PAL_POWER_RESET_REASON_DEEP_SLEEP,
  H2_PAL_POWER_RESET_REASON_PANIC,
  H2_PAL_POWER_RESET_REASON_BOOTLOADER,
} h2_pal_power_reset_reason_t;

typedef enum h2_pal_power_previous_transition {
  H2_PAL_POWER_PREVIOUS_TRANSITION_UNKNOWN = 0,
  H2_PAL_POWER_PREVIOUS_TRANSITION_NONE,
  H2_PAL_POWER_PREVIOUS_TRANSITION_SHUTDOWN,
  H2_PAL_POWER_PREVIOUS_TRANSITION_REBOOT,
  H2_PAL_POWER_PREVIOUS_TRANSITION_SLEEP,
  H2_PAL_POWER_PREVIOUS_TRANSITION_DEEP_SLEEP,
} h2_pal_power_previous_transition_t;

typedef struct h2_pal_power_boot_info {
  h2_pal_power_boot_source_t source;
  uint32_t source_id;
  h2_pal_power_previous_transition_t previous_transition;
  h2_pal_power_reset_reason_t reset_reason;
  uint32_t transition_reason;
  uint32_t boot_count;
} h2_pal_power_boot_info_t;

typedef struct h2_pal_power_boot_partition {
  uint32_t id;
  uint32_t flags;
  /**
   * NUL-terminated partition name.
   * Maximum length is H2_PAL_POWER_BOOT_PARTITION_NAME_MAX - 1.
   */
  char name[H2_PAL_POWER_BOOT_PARTITION_NAME_MAX];
} h2_pal_power_boot_partition_t;

/** Native boot policy, independent of a package or application role. */
typedef enum h2_pal_power_boot_policy {
  H2_PAL_POWER_BOOT_POLICY_EXISTING = 0,
  H2_PAL_POWER_BOOT_POLICY_TRIAL = 1,
  H2_PAL_POWER_BOOT_POLICY_TRIAL_WITH_RECOVERY = 2,
} h2_pal_power_boot_policy_t;

typedef struct h2_pal_power_boot_request {
  uint32_t partition_id;
  h2_pal_power_boot_policy_t policy;
} h2_pal_power_boot_request_t;

#define H2_PAL_POWER_NATIVE_EVIDENCE_MAX 64u

/** Versioned provider-defined native evidence in caller-owned storage. Callers
 * may persist and return these bytes but must not infer executable facts from
 * their contents. Empty
 * evidence uses format=0,size=0. No package role, hash, or preference key is
 * encoded here; any package binding belongs to the caller's envelope.
 */
typedef struct h2_pal_power_native_evidence {
  uint32_t format;
  uint32_t size;
  uint8_t bytes[H2_PAL_POWER_NATIVE_EVIDENCE_MAX];
} h2_pal_power_native_evidence_t;

typedef enum h2_pal_power_image_state_kind {
  H2_PAL_POWER_IMAGE_STATE_UNKNOWN = 0,
  H2_PAL_POWER_IMAGE_STATE_NEW = 1,
  H2_PAL_POWER_IMAGE_STATE_PENDING_VERIFY = 2,
  H2_PAL_POWER_IMAGE_STATE_VALID = 3,
  H2_PAL_POWER_IMAGE_STATE_INVALID = 4,
  H2_PAL_POWER_IMAGE_STATE_ABORTED = 5,
} h2_pal_power_image_state_kind_t;

typedef enum h2_pal_power_image_flag {
  H2_PAL_POWER_IMAGE_FLAG_CONFIRMATION_REQUIRED = 1u << 0,
  H2_PAL_POWER_IMAGE_FLAG_RECOVERY_REQUIRED = 1u << 1,
  H2_PAL_POWER_IMAGE_FLAG_RECOVERY_READY = 1u << 2,
  H2_PAL_POWER_IMAGE_FLAG_UNCONFIRMED_ATTEMPT = 1u << 3,
  H2_PAL_POWER_IMAGE_FLAG_RECOVERY_PUBLISHED = 1u << 4,
} h2_pal_power_image_flag_t;

typedef struct h2_pal_power_image_state {
  h2_pal_power_image_state_kind_t state;
  uint32_t flags;
} h2_pal_power_image_state_t;

typedef struct h2_pal_power_image_confirmation {
  uint32_t partition_id;
  uint32_t flags;
  uint8_t was_pending;
} h2_pal_power_image_confirmation_t;

typedef struct h2_pal_power_hold_state {
  int enabled;
} h2_pal_power_hold_state_t;

typedef struct h2_pal_power_state_event {
  h2_pal_power_state_t previous_state;
  h2_pal_power_state_t state;
  uint32_t reason;
  h2_pal_result_t result;
} h2_pal_power_state_event_t;

typedef struct h2_pal_power_transition_event {
  h2_pal_power_previous_transition_t transition;
  uint32_t reason;
  h2_pal_result_t result;
} h2_pal_power_transition_event_t;

typedef h2_pal_result_t (*h2_pal_power_boot_partition_cb_t)(
    void* user, const h2_pal_power_boot_partition_t* partition);

typedef struct h2_pal_power_vtable {
  h2_pal_result_t (*get_capabilities)(
      void* user, h2_pal_power_capabilities_t* out_capabilities);
  h2_pal_result_t (*get_boot_info)(void* user,
                                   h2_pal_power_boot_info_t* out_info);
  h2_pal_result_t (*get_state)(void* user, h2_pal_power_state_t* out_state);
  h2_pal_result_t (*list_boot_partitions)(void* user,
                                          h2_pal_power_boot_partition_cb_t cb,
                                          void* cb_user);
  h2_pal_result_t (*get_running_boot_partition)(
      void* user, h2_pal_power_boot_partition_t* out_partition);
  h2_pal_result_t (*get_next_boot_partition)(
      void* user, h2_pal_power_boot_partition_t* out_partition);
  h2_pal_result_t (*set_next_boot_partition)(void* user, uint32_t partition_id);
  h2_pal_result_t (*set_hold)(void* user, int enabled);
  h2_pal_result_t (*get_hold)(void* user, h2_pal_power_hold_state_t* out_state);
  h2_pal_result_t (*shutdown)(void* user, uint32_t reason);
  h2_pal_result_t (*reboot)(void* user, uint32_t reason);
  h2_pal_result_t (*sleep)(void* user, uint32_t reason);
  h2_pal_result_t (*deep_sleep)(void* user, uint32_t reason);
  h2_pal_result_t (*set_deep_sleep_wake_timer)(void* user, uint32_t delay_ms);
  /** Prepare native boot facts after destination verification. This does not
   * select, reboot, or publish a boot header. Evidence is caller-owned. A
   * duplicate request must be retry-safe or fail explicitly. EXISTING for the
   * running partition may return read-only native evidence without opening or
   * consuming an update transaction or changing the next boot selection. */
  h2_pal_result_t (*prepare_next_boot_partition)(
      void* user, const h2_pal_power_boot_request_t* request,
      h2_pal_power_native_evidence_t* out_evidence);
  h2_pal_result_t (*get_image_state)(void* user, uint32_t partition_id,
                                     h2_pal_power_image_state_t* out_state);
  /** Confirm only the running image, idempotently. A provider may require
   * evidence returned by prepare before reset. Selection of a confirmed
   * running recovery image through set_next remains the publication barrier
   * required before invalidating its former recovery partition. */
  h2_pal_result_t (*confirm_running_image)(
      void* user, const h2_pal_power_native_evidence_t* evidence,
      h2_pal_power_image_confirmation_t* out_confirmation);
  /** Refresh the configured hardware watchdog from its designated task.
   * Does not change timeout, enablement or watchdog ownership policy. */
  h2_pal_result_t (*feed_watchdog)(void* user);
} h2_pal_power_vtable_t;

typedef struct h2_pal_power_api {
  void* user;
  const h2_pal_power_vtable_t* vtable;
} h2_pal_power_api_t;

static inline h2_pal_result_t h2_pal_power_get_capabilities(
    const h2_pal_power_api_t* api,
    h2_pal_power_capabilities_t* out_capabilities) {
  if (out_capabilities == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (api == NULL || api->vtable == NULL ||
      api->vtable->get_capabilities == NULL) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  return api->vtable->get_capabilities(api->user, out_capabilities);
}

static inline h2_pal_result_t h2_pal_power_get_boot_info(
    const h2_pal_power_api_t* api, h2_pal_power_boot_info_t* out_info) {
  if (out_info == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (api == NULL || api->vtable == NULL ||
      api->vtable->get_boot_info == NULL) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  return api->vtable->get_boot_info(api->user, out_info);
}

static inline h2_pal_result_t h2_pal_power_get_state(
    const h2_pal_power_api_t* api, h2_pal_power_state_t* out_state) {
  if (out_state == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (api == NULL || api->vtable == NULL || api->vtable->get_state == NULL) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  return api->vtable->get_state(api->user, out_state);
}

static inline h2_pal_result_t h2_pal_power_list_boot_partitions(
    const h2_pal_power_api_t* api, h2_pal_power_boot_partition_cb_t cb,
    void* cb_user) {
  if (cb == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (api == NULL || api->vtable == NULL ||
      api->vtable->list_boot_partitions == NULL) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  return api->vtable->list_boot_partitions(api->user, cb, cb_user);
}

static inline h2_pal_result_t h2_pal_power_get_running_boot_partition(
    const h2_pal_power_api_t* api,
    h2_pal_power_boot_partition_t* out_partition) {
  if (out_partition == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (api == NULL || api->vtable == NULL ||
      api->vtable->get_running_boot_partition == NULL) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  return api->vtable->get_running_boot_partition(api->user, out_partition);
}

static inline h2_pal_result_t h2_pal_power_get_next_boot_partition(
    const h2_pal_power_api_t* api,
    h2_pal_power_boot_partition_t* out_partition) {
  if (out_partition == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (api == NULL || api->vtable == NULL ||
      api->vtable->get_next_boot_partition == NULL) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  return api->vtable->get_next_boot_partition(api->user, out_partition);
}

static inline h2_pal_result_t h2_pal_power_set_next_boot_partition(
    const h2_pal_power_api_t* api, uint32_t partition_id) {
  if (api == NULL || api->vtable == NULL ||
      api->vtable->set_next_boot_partition == NULL) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  return api->vtable->set_next_boot_partition(api->user, partition_id);
}

static inline h2_pal_result_t h2_pal_power_set_hold(
    const h2_pal_power_api_t* api, int enabled) {
  if (api == NULL || api->vtable == NULL || api->vtable->set_hold == NULL) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  return api->vtable->set_hold(api->user, enabled);
}

static inline h2_pal_result_t h2_pal_power_get_hold(
    const h2_pal_power_api_t* api, h2_pal_power_hold_state_t* out_state) {
  if (out_state == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (api == NULL || api->vtable == NULL || api->vtable->get_hold == NULL) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  return api->vtable->get_hold(api->user, out_state);
}

static inline h2_pal_result_t h2_pal_power_shutdown(
    const h2_pal_power_api_t* api, uint32_t reason) {
  if (api == NULL || api->vtable == NULL || api->vtable->shutdown == NULL) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  return api->vtable->shutdown(api->user, reason);
}

static inline h2_pal_result_t h2_pal_power_reboot(const h2_pal_power_api_t* api,
                                                  uint32_t reason) {
  if (api == NULL || api->vtable == NULL || api->vtable->reboot == NULL) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  return api->vtable->reboot(api->user, reason);
}

static inline h2_pal_result_t h2_pal_power_sleep(const h2_pal_power_api_t* api,
                                                 uint32_t reason) {
  if (api == NULL || api->vtable == NULL || api->vtable->sleep == NULL) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  return api->vtable->sleep(api->user, reason);
}

static inline h2_pal_result_t h2_pal_power_deep_sleep(
    const h2_pal_power_api_t* api, uint32_t reason) {
  if (api == NULL || api->vtable == NULL || api->vtable->deep_sleep == NULL) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  return api->vtable->deep_sleep(api->user, reason);
}

/**
 * Arm a wake timer for the next deep_sleep() call. The delay starts when
 * deep sleep is entered; 0 restores the provider's default wake policy. The
 * armed value stays in effect until it is changed or the device leaves deep
 * sleep. A timer wake reports
 * H2_PAL_POWER_BOOT_SOURCE_TIMER. Physical power off does not keep the timer.
 */
static inline h2_pal_result_t h2_pal_power_set_deep_sleep_wake_timer(
    const h2_pal_power_api_t* api, uint32_t delay_ms) {
  if (api == NULL || api->vtable == NULL ||
      api->vtable->set_deep_sleep_wake_timer == NULL) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  return api->vtable->set_deep_sleep_wake_timer(api->user, delay_ms);
}

static inline h2_pal_result_t h2_pal_power_prepare_next_boot_partition(
    const h2_pal_power_api_t* api, const h2_pal_power_boot_request_t* request,
    h2_pal_power_native_evidence_t* out_evidence) {
  h2_pal_result_t rc;
  if (out_evidence == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out_evidence, 0, sizeof(*out_evidence));
  if (request == NULL ||
      (request->policy != H2_PAL_POWER_BOOT_POLICY_EXISTING &&
       request->policy != H2_PAL_POWER_BOOT_POLICY_TRIAL &&
       request->policy != H2_PAL_POWER_BOOT_POLICY_TRIAL_WITH_RECOVERY))
    return H2_PAL_ERR_INVALID_ARG;
  if (api == NULL || api->vtable == NULL ||
      api->vtable->prepare_next_boot_partition == NULL)
    return H2_PAL_ERR_UNSUPPORTED;
  rc = api->vtable->prepare_next_boot_partition(api->user, request,
                                                out_evidence);
  if (rc == H2_PAL_OK &&
      (out_evidence->size > sizeof(out_evidence->bytes) ||
       (out_evidence->size == 0u) != (out_evidence->format == 0u)))
    rc = H2_PAL_ERR_FORMAT;
  if (rc != H2_PAL_OK) memset(out_evidence, 0, sizeof(*out_evidence));
  return rc;
}

static inline h2_pal_result_t h2_pal_power_get_image_state(
    const h2_pal_power_api_t* api, uint32_t partition_id,
    h2_pal_power_image_state_t* out_state) {
  h2_pal_result_t rc;
  if (out_state == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out_state, 0, sizeof(*out_state));
  if (api == NULL || api->vtable == NULL ||
      api->vtable->get_image_state == NULL)
    return H2_PAL_ERR_UNSUPPORTED;
  rc = api->vtable->get_image_state(api->user, partition_id, out_state);
  if (rc != H2_PAL_OK) memset(out_state, 0, sizeof(*out_state));
  return rc;
}

static inline h2_pal_result_t h2_pal_power_confirm_running_image(
    const h2_pal_power_api_t* api,
    const h2_pal_power_native_evidence_t* evidence,
    h2_pal_power_image_confirmation_t* out_confirmation) {
  h2_pal_result_t rc;
  if (out_confirmation == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out_confirmation, 0, sizeof(*out_confirmation));
  if (evidence != NULL && (evidence->size > sizeof(evidence->bytes) ||
                           (evidence->size == 0u) != (evidence->format == 0u)))
    return H2_PAL_ERR_INVALID_ARG;
  if (api == NULL || api->vtable == NULL ||
      api->vtable->confirm_running_image == NULL)
    return H2_PAL_ERR_UNSUPPORTED;
  rc =
      api->vtable->confirm_running_image(api->user, evidence, out_confirmation);
  if (rc != H2_PAL_OK) memset(out_confirmation, 0, sizeof(*out_confirmation));
  return rc;
}

static inline h2_pal_result_t h2_pal_power_feed_watchdog(
    const h2_pal_power_api_t* api) {
  if (api == NULL || api->vtable == NULL || api->vtable->feed_watchdog == NULL)
    return H2_PAL_ERR_UNSUPPORTED;
  return api->vtable->feed_watchdog(api->user);
}

#ifdef __cplusplus
}
#endif

#endif
