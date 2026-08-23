#include "h2_runtime_internal.h"

#include <string.h>

/*
 * State publication.
 *
 * A single writer (serialised by the owner, today the input writer mutex)
 * fills a retired slot and atomically switches the active index. Readers pin
 * the active slot with a per-slot reader count, so a publication never
 * blocks on a reader and a reader never observes a half-written bank. When
 * every retired slot is pinned the publication is deferred and the caller
 * retries on its next cycle.
 */

static void reset_publication(h2_runtime_state_publication_t *publication) {
    h2_runtime_state_entry_t *entries[H2_RUNTIME_STATE_SLOT_COUNT];
    size_t entry_capacities[H2_RUNTIME_STATE_SLOT_COUNT];
    for (size_t index = 0u; index < H2_RUNTIME_STATE_SLOT_COUNT; ++index) {
        entries[index] = publication->banks[index].entries;
        entry_capacities[index] = publication->banks[index].entry_capacity;
    }
    memset(publication, 0, sizeof(*publication));
    atomic_init(&publication->ready, 0);
    atomic_init(&publication->active_index, 0u);
    atomic_flag_clear(&publication->reader_lock);
    for (size_t index = 0u; index < H2_RUNTIME_STATE_SLOT_COUNT; ++index) {
        atomic_init(&publication->reader_count[index], 0u);
        publication->banks[index].entries = entries[index];
        publication->banks[index].entry_capacity = entry_capacities[index];
    }
}

h2_pal_result_t h2_runtime_state_publication_init(h2_runtime_t *runtime) {
    if (runtime == NULL || runtime->private_state == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_runtime_state_publication_t *publication =
        &runtime->private_state->state_publication;
    if (atomic_load_explicit(&publication->ready, memory_order_acquire) != 0) {
        return H2_PAL_OK;
    }
    reset_publication(publication);
    atomic_store_explicit(&publication->ready, 1, memory_order_release);
    return H2_PAL_OK;
}

void h2_runtime_state_publication_deinit(h2_runtime_t *runtime) {
    if (runtime == NULL || runtime->private_state == NULL) {
        return;
    }
    h2_runtime_state_publication_t *publication =
        &runtime->private_state->state_publication;
    if (atomic_exchange_explicit(
            &publication->ready, 0, memory_order_acq_rel) == 0) {
        return;
    }
    reset_publication(publication);
}

int h2_runtime_state_publication_ready(const h2_runtime_t *runtime) {
    return runtime != NULL && runtime->private_state != NULL &&
           atomic_load_explicit(
               &runtime->private_state->state_publication.ready,
               memory_order_acquire) != 0;
}

void h2_runtime_state_set_publish_interval(
    h2_runtime_t *runtime,
    uint32_t interval_ms) {
    runtime->private_state->state_publish_interval_ms =
        interval_ms != 0u ? interval_ms : H2_RUNTIME_STATE_PUBLISH_INTERVAL_MS;
}

void h2_runtime_state_mark_dirty(h2_runtime_t *runtime) {
    runtime->private_state->state_dirty = 1;
}

/*
 * reader_count arithmetic runs under a short test-and-set lock so it only
 * needs atomic load/store and exchange, which every supported target
 * (including ARMv5) lowers natively.
 */
static unsigned int reader_count_add(
    h2_runtime_state_publication_t *publication,
    unsigned int slot_index,
    int delta) {
    while (atomic_flag_test_and_set_explicit(
        &publication->reader_lock, memory_order_acquire)) {
    }
    const unsigned int previous = atomic_load_explicit(
        &publication->reader_count[slot_index], memory_order_relaxed);
    atomic_store_explicit(
        &publication->reader_count[slot_index],
        (unsigned int)((int)previous + delta),
        memory_order_relaxed);
    atomic_flag_clear_explicit(
        &publication->reader_lock, memory_order_release);
    return previous;
}

h2_pal_result_t h2_runtime_state_read_begin(
    const h2_runtime_t *runtime,
    const h2_runtime_state_bank_t **out_bank,
    uint8_t *out_slot_index) {
    if (!h2_runtime_ready(runtime) || out_bank == NULL ||
        out_slot_index == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_bank = NULL;
    *out_slot_index = UINT8_MAX;

    h2_runtime_state_publication_t *publication =
        &runtime->private_state->state_publication;
    if (atomic_load_explicit(&publication->ready, memory_order_acquire) == 0) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    for (;;) {
        const unsigned int slot_index = atomic_load_explicit(
            &publication->active_index, memory_order_acquire);
        if (slot_index >= H2_RUNTIME_STATE_SLOT_COUNT) {
            return H2_PAL_ERR_INVALID_STATE;
        }
        (void)reader_count_add(publication, slot_index, 1);
        if (atomic_load_explicit(&publication->ready,
                                 memory_order_acquire) != 0 &&
            slot_index == atomic_load_explicit(
                              &publication->active_index,
                              memory_order_acquire)) {
            *out_bank = &publication->banks[slot_index];
            *out_slot_index = (uint8_t)slot_index;
            return H2_PAL_OK;
        }
        (void)reader_count_add(publication, slot_index, -1);
        if (atomic_load_explicit(&publication->ready,
                                 memory_order_acquire) == 0) {
            return H2_PAL_ERR_NOT_FOUND;
        }
    }
}

h2_pal_result_t h2_runtime_state_read_end(
    const h2_runtime_t *runtime,
    uint8_t slot_index) {
    if (!h2_runtime_ready(runtime) ||
        slot_index >= H2_RUNTIME_STATE_SLOT_COUNT) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_runtime_state_publication_t *publication =
        &runtime->private_state->state_publication;
    if (atomic_load_explicit(&publication->ready, memory_order_acquire) == 0) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    unsigned int previous = reader_count_add(publication, slot_index, -1);
    if (previous == 0u) {
        (void)reader_count_add(publication, slot_index, 1);
        return H2_PAL_ERR_INVALID_STATE;
    }
    return H2_PAL_OK;
}

static void prepare_bank(
    h2_runtime_t *runtime,
    h2_runtime_state_bank_t *bank,
    h2_runtime_state_fill_fn fill,
    h2_runtime_sequence_t event_sequence_ceiling) {
    h2_runtime_state_entry_t *entries = bank->entries;
    size_t entry_capacity = bank->entry_capacity;
    memset(entries, 0, entry_capacity * sizeof(*entries));
    memset(bank, 0, sizeof(*bank));
    bank->entries = entries;
    bank->entry_capacity = entry_capacity;
    runtime->private_state->state_generation += 1u;
    bank->generation = runtime->private_state->state_generation;
    bank->event_sequence_ceiling = event_sequence_ceiling;
    if (fill != NULL) {
        fill(runtime, bank);
    }
}

h2_pal_result_t h2_runtime_state_publish(
    h2_runtime_t *runtime,
    h2_runtime_state_fill_fn fill,
    h2_runtime_sequence_t event_sequence_ceiling) {
    h2_runtime_state_publication_t *publication =
        &runtime->private_state->state_publication;
    const unsigned int active_index = atomic_load_explicit(
        &publication->active_index, memory_order_acquire);
    if (active_index >= H2_RUNTIME_STATE_SLOT_COUNT) {
        return H2_PAL_ERR_INVALID_STATE;
    }

    unsigned int next_index = H2_RUNTIME_STATE_SLOT_COUNT;
    for (unsigned int offset = 1u; offset < H2_RUNTIME_STATE_SLOT_COUNT;
         ++offset) {
        const unsigned int candidate =
            (active_index + offset) % H2_RUNTIME_STATE_SLOT_COUNT;
        if (atomic_load_explicit(
                &publication->reader_count[candidate],
                memory_order_acquire) == 0u) {
            next_index = candidate;
            break;
        }
    }
    if (next_index == H2_RUNTIME_STATE_SLOT_COUNT) {
        publication->deferred_count += 1u;
        return H2_PAL_ERR_WOULD_BLOCK;
    }

    prepare_bank(
        runtime, &publication->banks[next_index], fill, event_sequence_ceiling);
    publication->copy_count += 1u;
    atomic_store_explicit(
        &publication->active_index, next_index, memory_order_release);
    publication->switch_count += 1u;
    runtime->private_state->state_dirty = 0;
    return H2_PAL_OK;
}

h2_pal_result_t h2_runtime_state_publish_if_due(
    h2_runtime_t *runtime,
    h2_runtime_timestamp_ms_t now_ms,
    int force,
    h2_runtime_state_fill_fn fill,
    h2_runtime_sequence_t event_sequence_ceiling) {
    if (force == 0 &&
        h2_pal_time_deadline_expired(
            now_ms, runtime->private_state->state_next_publish_ms) == 0) {
        return H2_PAL_OK;
    }
    runtime->private_state->state_next_publish_ms = h2_pal_time_deadline_ms(
        now_ms, runtime->private_state->state_publish_interval_ms);
    if (runtime->private_state->state_dirty == 0) {
        return H2_PAL_OK;
    }
    h2_pal_result_t rc =
        h2_runtime_state_publish(runtime, fill, event_sequence_ceiling);
    return rc == H2_PAL_ERR_WOULD_BLOCK ? H2_PAL_OK : rc;
}

/* Component-state readers. */

static const h2_runtime_state_entry_t *find_input_state(
    const h2_runtime_state_bank_t *bank,
    h2_runtime_component_t component,
    h2_runtime_component_id_t component_id) {
    for (size_t i = 0u; i < bank->entry_count; ++i) {
        const h2_runtime_state_entry_t *entry =
            &bank->entries[i];
        if (entry->component == component &&
            entry->component_id == component_id) {
            return entry;
        }
    }
    return NULL;
}

static h2_pal_result_t read_button_state(
    const h2_runtime_t *runtime,
    h2_runtime_component_id_t component_id,
    h2_runtime_button_state_t *out_state) {
    const h2_runtime_state_bank_t *bank = NULL;
    uint8_t slot_index = UINT8_MAX;
    h2_pal_result_t rc =
        h2_runtime_state_read_begin(runtime, &bank, &slot_index);
    if (rc != H2_PAL_OK) {
        return rc;
    }

    h2_runtime_button_state_t state = {0};
    const h2_runtime_state_entry_t *entry =
        find_input_state(
            bank,
            H2_RUNTIME_COMPONENT_BUTTON,
            component_id);
    int found = entry != NULL;
    if (found != 0) {
        state = entry->state.button;
    }
    h2_pal_result_t release_rc =
        h2_runtime_state_read_end(runtime, slot_index);
    if (release_rc != H2_PAL_OK) {
        return release_rc;
    }
    if (found == 0) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    *out_state = state;
    return H2_PAL_OK;
}

static h2_pal_result_t read_nfc_state(
    const h2_runtime_t *runtime,
    h2_runtime_component_id_t component_id,
    h2_runtime_nfc_state_t *out_state) {
    const h2_runtime_state_bank_t *bank = NULL;
    uint8_t slot_index = UINT8_MAX;
    h2_pal_result_t rc =
        h2_runtime_state_read_begin(runtime, &bank, &slot_index);
    if (rc != H2_PAL_OK) {
        return rc;
    }

    h2_runtime_nfc_state_t state = {0};
    const h2_runtime_state_entry_t *entry =
        find_input_state(
            bank,
            H2_RUNTIME_COMPONENT_NFC_READER,
            component_id);
    int found = entry != NULL;
    if (found != 0) {
        state = entry->state.nfc;
    }
    h2_pal_result_t release_rc =
        h2_runtime_state_read_end(runtime, slot_index);
    if (release_rc != H2_PAL_OK) {
        return release_rc;
    }
    if (found == 0) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    *out_state = state;
    return H2_PAL_OK;
}

static h2_pal_result_t read_imu_state(
    const h2_runtime_t *runtime,
    h2_runtime_component_id_t component_id,
    h2_runtime_imu_state_t *out_state) {
    const h2_runtime_state_bank_t *bank = NULL;
    uint8_t slot_index = UINT8_MAX;
    h2_pal_result_t rc =
        h2_runtime_state_read_begin(runtime, &bank, &slot_index);
    if (rc != H2_PAL_OK) {
        return rc;
    }

    h2_runtime_imu_state_t state = {0};
    const h2_runtime_state_entry_t *entry =
        find_input_state(
            bank,
            H2_RUNTIME_COMPONENT_IMU,
            component_id);
    int found = entry != NULL;
    if (found != 0) {
        state = entry->state.imu;
    }
    h2_pal_result_t release_rc =
        h2_runtime_state_read_end(runtime, slot_index);
    if (release_rc != H2_PAL_OK) {
        return release_rc;
    }
    if (found == 0) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    *out_state = state;
    return H2_PAL_OK;
}

static h2_pal_result_t read_battery_state(
    const h2_runtime_t *runtime,
    h2_runtime_component_id_t component_id,
    h2_runtime_battery_state_t *out_state) {
    const h2_runtime_state_bank_t *bank = NULL;
    uint8_t slot_index = UINT8_MAX;
    h2_pal_result_t rc = h2_runtime_state_read_begin(
        runtime, &bank, &slot_index);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    h2_runtime_battery_state_t state = {0};
    const h2_runtime_state_entry_t *entry = find_input_state(
        bank, H2_RUNTIME_COMPONENT_BATTERY, component_id);
    const int found = entry != NULL;
    if (found != 0) {
        state = entry->state.battery;
    }
    h2_pal_result_t release_rc = h2_runtime_state_read_end(
        runtime, slot_index);
    if (release_rc != H2_PAL_OK) {
        return release_rc;
    }
    if (found == 0) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    *out_state = state;
    return H2_PAL_OK;
}

static h2_pal_result_t read_temperature_state(
    const h2_runtime_t *runtime,
    h2_runtime_component_id_t component_id,
    h2_runtime_temperature_state_t *out_state) {
    const h2_runtime_state_bank_t *bank = NULL;
    uint8_t slot_index = UINT8_MAX;
    h2_pal_result_t rc = h2_runtime_state_read_begin(
        runtime, &bank, &slot_index);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    h2_runtime_temperature_state_t state = {0};
    const h2_runtime_state_entry_t *entry = find_input_state(
        bank, H2_RUNTIME_COMPONENT_TEMPERATURE_SENSOR, component_id);
    const int found = entry != NULL;
    if (found != 0) {
        state = entry->state.temperature;
    }
    h2_pal_result_t release_rc = h2_runtime_state_read_end(
        runtime, slot_index);
    if (release_rc != H2_PAL_OK) {
        return release_rc;
    }
    if (found == 0) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    *out_state = state;
    return H2_PAL_OK;
}

h2_pal_result_t h2_runtime_component_state_button(
    const h2_runtime_t *runtime,
    h2_runtime_component_id_t component_id,
    h2_runtime_button_state_t *out_state) {
    if (out_state == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_state, 0, sizeof(*out_state));
    if (!h2_runtime_ready(runtime) ||
        component_id == H2_RUNTIME_COMPONENT_ID_NONE) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return read_button_state(runtime, component_id, out_state);
}

h2_pal_result_t h2_runtime_component_state_nfc(
    const h2_runtime_t *runtime,
    h2_runtime_component_id_t component_id,
    h2_runtime_nfc_state_t *out_state) {
    if (out_state == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_state, 0, sizeof(*out_state));
    if (!h2_runtime_ready(runtime) ||
        component_id == H2_RUNTIME_COMPONENT_ID_NONE) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return read_nfc_state(runtime, component_id, out_state);
}

h2_pal_result_t h2_runtime_component_state_imu(
    const h2_runtime_t *runtime,
    h2_runtime_component_id_t component_id,
    h2_runtime_imu_state_t *out_state) {
    if (out_state == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_state, 0, sizeof(*out_state));
    if (!h2_runtime_ready(runtime) ||
        component_id == H2_RUNTIME_COMPONENT_ID_NONE) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return read_imu_state(runtime, component_id, out_state);
}

h2_pal_result_t h2_runtime_component_state_battery(
    const h2_runtime_t *runtime,
    h2_runtime_component_id_t component_id,
    h2_runtime_battery_state_t *out_state) {
    if (out_state == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_state, 0, sizeof(*out_state));
    if (!h2_runtime_ready(runtime) ||
        component_id == H2_RUNTIME_COMPONENT_ID_NONE) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return read_battery_state(runtime, component_id, out_state);
}

h2_pal_result_t h2_runtime_component_state_temperature(
    const h2_runtime_t *runtime,
    h2_runtime_component_id_t component_id,
    h2_runtime_temperature_state_t *out_state) {
    if (out_state == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_state, 0, sizeof(*out_state));
    if (!h2_runtime_ready(runtime) ||
        component_id == H2_RUNTIME_COMPONENT_ID_NONE) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return read_temperature_state(runtime, component_id, out_state);
}

static h2_pal_result_t unsupported_component_state(
    const h2_runtime_t *runtime,
    void *out_state,
    size_t state_size) {
    if (!h2_runtime_ready(runtime) || out_state == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_state, 0, state_size);
    return H2_PAL_ERR_UNSUPPORTED;
}

h2_pal_result_t h2_runtime_component_state_display(
    const h2_runtime_t *runtime,
    h2_runtime_component_state_display_t *out_state) {
    return unsupported_component_state(runtime, out_state, sizeof(*out_state));
}

h2_pal_result_t h2_runtime_component_state_audio(
    const h2_runtime_t *runtime,
    h2_runtime_component_state_audio_t *out_state) {
    return unsupported_component_state(runtime, out_state, sizeof(*out_state));
}

h2_pal_result_t h2_runtime_component_state_power(
    const h2_runtime_t *runtime,
    h2_runtime_component_state_power_t *out_state) {
    return unsupported_component_state(runtime, out_state, sizeof(*out_state));
}

h2_pal_result_t h2_runtime_component_state_netif(
    const h2_runtime_t *runtime,
    h2_runtime_component_state_netif_t *out_state) {
    return unsupported_component_state(runtime, out_state, sizeof(*out_state));
}

h2_pal_result_t h2_runtime_component_state_led(
    const h2_runtime_t *runtime,
    h2_runtime_component_state_led_t *out_state) {
    return unsupported_component_state(runtime, out_state, sizeof(*out_state));
}

h2_pal_result_t h2_runtime_component_state_switch(
    const h2_runtime_t *runtime,
    h2_runtime_component_state_switch_t *out_state) {
    return unsupported_component_state(runtime, out_state, sizeof(*out_state));
}

h2_pal_result_t h2_runtime_component_state_pwm_switch(
    const h2_runtime_t *runtime,
    h2_runtime_component_state_pwm_switch_t *out_state) {
    return unsupported_component_state(runtime, out_state, sizeof(*out_state));
}
