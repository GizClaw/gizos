#ifndef H2_PAL_POWER_H
#define H2_PAL_POWER_H

#include "h2/pal/core/h2_pal_errors.h"

#include <stddef.h>
#include <stdint.h>

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
    void *user,
    const h2_pal_power_boot_partition_t *partition);

typedef struct h2_pal_power_vtable {
    h2_pal_result_t (*get_capabilities)(
        void *user,
        h2_pal_power_capabilities_t *out_capabilities);
    h2_pal_result_t (*get_boot_info)(
        void *user,
        h2_pal_power_boot_info_t *out_info);
    h2_pal_result_t (*get_state)(
        void *user,
        h2_pal_power_state_t *out_state);
    h2_pal_result_t (*list_boot_partitions)(
        void *user,
        h2_pal_power_boot_partition_cb_t cb,
        void *cb_user);
    h2_pal_result_t (*get_running_boot_partition)(
        void *user,
        h2_pal_power_boot_partition_t *out_partition);
    h2_pal_result_t (*get_next_boot_partition)(
        void *user,
        h2_pal_power_boot_partition_t *out_partition);
    h2_pal_result_t (*set_next_boot_partition)(
        void *user,
        uint32_t partition_id);
    h2_pal_result_t (*set_hold)(
        void *user,
        int enabled);
    h2_pal_result_t (*get_hold)(
        void *user,
        h2_pal_power_hold_state_t *out_state);
    h2_pal_result_t (*shutdown)(void *user, uint32_t reason);
    h2_pal_result_t (*reboot)(void *user, uint32_t reason);
    h2_pal_result_t (*sleep)(void *user, uint32_t reason);
    h2_pal_result_t (*deep_sleep)(void *user, uint32_t reason);
} h2_pal_power_vtable_t;

typedef struct h2_pal_power_api {
    void *user;
    const h2_pal_power_vtable_t *vtable;
} h2_pal_power_api_t;

static inline h2_pal_result_t h2_pal_power_get_capabilities(
    const h2_pal_power_api_t *api,
    h2_pal_power_capabilities_t *out_capabilities) {
    if (out_capabilities == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->get_capabilities == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->get_capabilities(api->user, out_capabilities);
}

static inline h2_pal_result_t h2_pal_power_get_boot_info(
    const h2_pal_power_api_t *api,
    h2_pal_power_boot_info_t *out_info) {
    if (out_info == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->get_boot_info == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->get_boot_info(api->user, out_info);
}

static inline h2_pal_result_t h2_pal_power_get_state(
    const h2_pal_power_api_t *api,
    h2_pal_power_state_t *out_state) {
    if (out_state == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->get_state == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->get_state(api->user, out_state);
}

static inline h2_pal_result_t h2_pal_power_list_boot_partitions(
    const h2_pal_power_api_t *api,
    h2_pal_power_boot_partition_cb_t cb,
    void *cb_user) {
    if (cb == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->list_boot_partitions == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->list_boot_partitions(api->user, cb, cb_user);
}

static inline h2_pal_result_t h2_pal_power_get_running_boot_partition(
    const h2_pal_power_api_t *api,
    h2_pal_power_boot_partition_t *out_partition) {
    if (out_partition == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->get_running_boot_partition == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->get_running_boot_partition(api->user, out_partition);
}

static inline h2_pal_result_t h2_pal_power_get_next_boot_partition(
    const h2_pal_power_api_t *api,
    h2_pal_power_boot_partition_t *out_partition) {
    if (out_partition == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->get_next_boot_partition == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->get_next_boot_partition(api->user, out_partition);
}

static inline h2_pal_result_t h2_pal_power_set_next_boot_partition(
    const h2_pal_power_api_t *api,
    uint32_t partition_id) {
    if (api == NULL || api->vtable == NULL || api->vtable->set_next_boot_partition == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->set_next_boot_partition(api->user, partition_id);
}

static inline h2_pal_result_t h2_pal_power_set_hold(
    const h2_pal_power_api_t *api,
    int enabled) {
    if (api == NULL || api->vtable == NULL || api->vtable->set_hold == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->set_hold(api->user, enabled);
}

static inline h2_pal_result_t h2_pal_power_get_hold(
    const h2_pal_power_api_t *api,
    h2_pal_power_hold_state_t *out_state) {
    if (out_state == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->get_hold == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->get_hold(api->user, out_state);
}

static inline h2_pal_result_t h2_pal_power_shutdown(
    const h2_pal_power_api_t *api,
    uint32_t reason) {
    if (api == NULL || api->vtable == NULL || api->vtable->shutdown == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->shutdown(api->user, reason);
}

static inline h2_pal_result_t h2_pal_power_reboot(
    const h2_pal_power_api_t *api,
    uint32_t reason) {
    if (api == NULL || api->vtable == NULL || api->vtable->reboot == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->reboot(api->user, reason);
}

static inline h2_pal_result_t h2_pal_power_sleep(
    const h2_pal_power_api_t *api,
    uint32_t reason) {
    if (api == NULL || api->vtable == NULL || api->vtable->sleep == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->sleep(api->user, reason);
}

static inline h2_pal_result_t h2_pal_power_deep_sleep(
    const h2_pal_power_api_t *api,
    uint32_t reason) {
    if (api == NULL || api->vtable == NULL || api->vtable->deep_sleep == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->deep_sleep(api->user, reason);
}

#ifdef __cplusplus
}
#endif

#endif
