#include "h2_bk3633_platform_core.h"

#include "h2_libco.h"

#include <stdatomic.h>
#include <string.h>

#if defined(BK3633)
#include "arch.h"
#endif

typedef enum h2_bk3633_completion_state {
    H2_BK3633_COMPLETION_FREE = 0,
    H2_BK3633_COMPLETION_PENDING,
    H2_BK3633_COMPLETION_DELIVERED,
} h2_bk3633_completion_state_t;

typedef struct h2_bk3633_completion_slot {
    uintptr_t wait_key;
    uint64_t order;
    h2_bk3633_completion_state_t state;
} h2_bk3633_completion_slot_t;

static h2_libco_t *volatile s_libco;
static const h2_pal_mem_api_t *s_completion_allocator;
static h2_bk3633_completion_slot_t *volatile s_completion_slots;
static volatile size_t s_completion_capacity;
static volatile uint64_t s_completion_order;
static volatile int s_completion_full_fault;

static uint32_t completion_critical_enter(void) {
#if defined(BK3633)
    uint32_t state = __disable_fiq() != 0 ? 1u : 0u;
    if (__disable_irq() != 0) {
        state |= 2u;
    }
    return state;
#else
    return 0u;
#endif
}

static void completion_critical_exit(uint32_t state) {
#if defined(BK3633)
    if ((state & 1u) == 0u) {
        __enable_fiq();
    }
    if ((state & 2u) == 0u) {
        __enable_irq();
    }
#else
    (void)state;
#endif
}

h2_bk3633_platform_libco_result_t
h2_bk3633_platform_libco_bind(
    const h2_bk3633_platform_libco_config_t *config) {
    h2_bk3633_completion_slot_t *slots;
    uint32_t critical_state;
    size_t bytes;
    if (config == NULL || config->executor == NULL ||
        config->allocator == NULL || config->allocator->vtable == NULL ||
        config->allocator->vtable->alloc == NULL ||
        config->allocator->vtable->free == NULL ||
        config->completion_capacity == 0u ||
        config->completion_capacity >
            SIZE_MAX / sizeof(h2_bk3633_completion_slot_t)) {
        return H2_BK3633_PLATFORM_LIBCO_ERR_INVALID_ARG;
    }
    critical_state = completion_critical_enter();
    if (s_libco != NULL) {
        completion_critical_exit(critical_state);
        return H2_BK3633_PLATFORM_LIBCO_ERR_INVALID_STATE;
    }
    completion_critical_exit(critical_state);

    bytes = config->completion_capacity *
            sizeof(h2_bk3633_completion_slot_t);
    slots = h2_pal_mem_alloc(config->allocator, bytes);
    if (slots == NULL) {
        return H2_BK3633_PLATFORM_LIBCO_ERR_NO_MEMORY;
    }
    memset(slots, 0, bytes);

    critical_state = completion_critical_enter();
    if (s_libco != NULL) {
        completion_critical_exit(critical_state);
        h2_pal_mem_free(config->allocator, slots);
        return H2_BK3633_PLATFORM_LIBCO_ERR_INVALID_STATE;
    }
    s_completion_allocator = config->allocator;
    s_completion_slots = slots;
    s_completion_capacity = config->completion_capacity;
    s_completion_order = 0u;
    s_completion_full_fault = 0;
    s_libco = config->executor;
    completion_critical_exit(critical_state);
    return H2_BK3633_PLATFORM_LIBCO_OK;
}

void h2_bk3633_platform_libco_unbind(void) {
    const h2_pal_mem_api_t *allocator;
    h2_bk3633_completion_slot_t *slots;
    uint32_t critical_state = completion_critical_enter();
    s_libco = NULL;
    slots = s_completion_slots;
    allocator = s_completion_allocator;
    s_completion_slots = NULL;
    s_completion_allocator = NULL;
    s_completion_capacity = 0u;
    s_completion_order = 0u;
    s_completion_full_fault = 0;
    completion_critical_exit(critical_state);
    h2_pal_mem_free(allocator, slots);
}

h2_bk3633_platform_libco_result_t
h2_bk3633_platform_libco_record_completion(uintptr_t wait_key) {
    h2_bk3633_platform_libco_result_t result =
        H2_BK3633_PLATFORM_LIBCO_OK;
    uint32_t critical_state;
    size_t free_index = SIZE_MAX;
    if (wait_key == 0u) {
        return H2_BK3633_PLATFORM_LIBCO_ERR_INVALID_ARG;
    }
    critical_state = completion_critical_enter();
    if (s_libco == NULL) {
        result = H2_BK3633_PLATFORM_LIBCO_ERR_INVALID_STATE;
    } else {
        for (size_t index = 0u; index < s_completion_capacity; ++index) {
            h2_bk3633_completion_slot_t *slot =
                &s_completion_slots[index];
            if (slot->state != H2_BK3633_COMPLETION_FREE &&
                slot->wait_key == wait_key) {
                completion_critical_exit(critical_state);
                return H2_BK3633_PLATFORM_LIBCO_OK;
            }
            if (slot->state == H2_BK3633_COMPLETION_FREE &&
                free_index == SIZE_MAX) {
                free_index = index;
            }
        }
        if (free_index == SIZE_MAX) {
            s_completion_full_fault = 1;
            result = H2_BK3633_PLATFORM_LIBCO_ERR_FULL;
        } else {
            h2_bk3633_completion_slot_t *slot =
                &s_completion_slots[free_index];
            slot->wait_key = wait_key;
            slot->order = ++s_completion_order;
            atomic_signal_fence(memory_order_release);
            slot->state = H2_BK3633_COMPLETION_PENDING;
        }
    }
    completion_critical_exit(critical_state);
    return result;
}

h2_bk3633_platform_libco_result_t
h2_bk3633_platform_libco_dispatch_wakes(size_t work_budget,
                                        size_t *out_dispatched) {
    size_t dispatched = 0u;
    if (out_dispatched != NULL) {
        *out_dispatched = 0u;
    }
    if (s_libco == NULL) {
        return H2_BK3633_PLATFORM_LIBCO_ERR_INVALID_STATE;
    }
    while (dispatched < work_budget) {
        uint32_t critical_state = completion_critical_enter();
        size_t selected = SIZE_MAX;
        uint64_t selected_order = 0u;
        h2_libco_t *core = s_libco;
        if (core == NULL) {
            completion_critical_exit(critical_state);
            return H2_BK3633_PLATFORM_LIBCO_ERR_INVALID_STATE;
        }
        for (size_t index = 0u; index < s_completion_capacity; ++index) {
            h2_bk3633_completion_slot_t *slot =
                &s_completion_slots[index];
            if (slot->state == H2_BK3633_COMPLETION_PENDING &&
                (selected == SIZE_MAX || slot->order < selected_order)) {
                selected = index;
                selected_order = slot->order;
            }
        }
        if (selected == SIZE_MAX) {
            completion_critical_exit(critical_state);
            break;
        }
        atomic_signal_fence(memory_order_acquire);
        uintptr_t wait_key = s_completion_slots[selected].wait_key;
        completion_critical_exit(critical_state);
        size_t woken = 0u;
        if (h2_libco_wake(core, wait_key, 1u, &woken) !=
            H2_LIBCO_OK) {
            return H2_BK3633_PLATFORM_LIBCO_ERR_WAKE;
        }
        critical_state = completion_critical_enter();
        h2_bk3633_completion_slot_t *slot =
            &s_completion_slots[selected];
        if (slot->state == H2_BK3633_COMPLETION_PENDING &&
            slot->wait_key == wait_key) {
            if (woken == 1u) {
                slot->state = H2_BK3633_COMPLETION_DELIVERED;
            } else {
                memset(slot, 0, sizeof(*slot));
            }
        }
        completion_critical_exit(critical_state);
        ++dispatched;
    }
    if (out_dispatched != NULL) {
        *out_dispatched = dispatched;
    }
    uint32_t critical_state = completion_critical_enter();
    int full_fault = s_completion_full_fault;
    s_completion_full_fault = 0;
    completion_critical_exit(critical_state);
    if (full_fault != 0) {
        return H2_BK3633_PLATFORM_LIBCO_ERR_FULL;
    }
    return H2_BK3633_PLATFORM_LIBCO_OK;
}

h2_pal_result_t h2_bk3633_platform_libco_wait(uintptr_t wait_key,
                                              uint32_t timeout_ms) {
    if (s_libco == NULL || wait_key == 0u) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    h2_libco_result_t result = h2_libco_wait(s_libco, wait_key, timeout_ms);
    uint32_t critical_state = completion_critical_enter();
    if (s_libco != NULL) {
        for (size_t index = 0u; index < s_completion_capacity; ++index) {
            h2_bk3633_completion_slot_t *slot =
                &s_completion_slots[index];
            if (slot->state == H2_BK3633_COMPLETION_DELIVERED &&
                slot->wait_key == wait_key) {
                memset(slot, 0, sizeof(*slot));
                break;
            }
        }
    }
    completion_critical_exit(critical_state);
    switch (result) {
    case H2_LIBCO_OK:
    case H2_LIBCO_WOKEN:
        return H2_PAL_OK;
    case H2_LIBCO_ERR_TIMEOUT:
        return H2_PAL_ERR_TIMEOUT;
    case H2_LIBCO_ERR_CANCELLED:
        return H2_PAL_EXIT;
    case H2_LIBCO_ERR_INVALID_ARG:
        return H2_PAL_ERR_INVALID_ARG;
    default:
        return H2_PAL_ERR_INVALID_STATE;
    }
}

bool h2_bk3633_platform_libco_has_pending(void) {
    uint32_t critical_state = completion_critical_enter();
    bool pending = s_completion_full_fault != 0;
    if (!pending && s_libco != NULL) {
        for (size_t index = 0u; index < s_completion_capacity; ++index) {
            if (s_completion_slots[index].state ==
                H2_BK3633_COMPLETION_PENDING) {
                pending = true;
                break;
            }
        }
    }
    completion_critical_exit(critical_state);
    return pending;
}
