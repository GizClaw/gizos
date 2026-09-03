#include "h2_runtime_internal.h"

#include <stdint.h>
#include <string.h>

typedef struct h2_runtime_private_layout {
    size_t allocation_size;
    size_t component_mappings_offset;
    size_t input_sources_offset;
    size_t input_pending_events_offset;
    size_t state_entries_offset[H2_RUNTIME_STATE_SLOT_COUNT];
    size_t component_mapping_capacity;
    size_t input_source_capacity;
    size_t input_pending_event_capacity;
    size_t event_payload_capacity;
} h2_runtime_private_layout_t;

static int append_private_storage(
    size_t item_count,
    size_t item_size,
    size_t item_alignment,
    size_t *inout_size,
    size_t *out_offset) {
    size_t size = *inout_size;
    size_t remainder = size % item_alignment;
    size_t padding = remainder == 0u ? 0u : item_alignment - remainder;
    if (size > SIZE_MAX - padding ||
        item_count > (SIZE_MAX - size - padding) / item_size) {
        return 0;
    }
    size += padding;
    *out_offset = size;
    *inout_size = size + item_count * item_size;
    return 1;
}

static int runtime_private_layout(
    const h2_runtime_config_t *config,
    h2_runtime_private_layout_t *out_layout) {
    h2_runtime_private_layout_t layout = {
        .allocation_size = sizeof(h2_runtime_private_t),
        .component_mapping_capacity =
            config->component_mapping_capacity == 0u
                ? H2_RUNTIME_DEFAULT_COMPONENT_MAPPING_CAPACITY
                : config->component_mapping_capacity,
        .input_source_capacity =
            config->input_source_capacity == 0u
                ? H2_RUNTIME_DEFAULT_INPUT_SOURCE_CAPACITY
                : config->input_source_capacity,
        .event_payload_capacity =
            config->event_payload_capacity == 0u
                ? H2_RUNTIME_EVENT_PAYLOAD_MAX
                : config->event_payload_capacity,
    };
    layout.input_pending_event_capacity =
        layout.input_source_capacity <
                H2_RUNTIME_RADIO_BUTTON_TRANSITION_EVENT_MAX
            ? H2_RUNTIME_RADIO_BUTTON_TRANSITION_EVENT_MAX
            : layout.input_source_capacity;

    if (layout.input_source_capacity == 0u ||
        layout.component_mapping_capacity == 0u ||
        layout.event_payload_capacity > H2_RUNTIME_EVENT_PAYLOAD_MAX ||
        layout.event_payload_capacity <
            h2_runtime_system_event_payload_capacity_min() ||
        !append_private_storage(
            layout.component_mapping_capacity,
            sizeof(h2_runtime_component_mapping_t),
            _Alignof(h2_runtime_component_mapping_t),
            &layout.allocation_size,
            &layout.component_mappings_offset) ||
        !append_private_storage(
            layout.input_source_capacity,
            sizeof(h2_runtime_input_source_t),
            _Alignof(h2_runtime_input_source_t),
            &layout.allocation_size,
            &layout.input_sources_offset) ||
        !append_private_storage(
            layout.input_pending_event_capacity,
            sizeof(h2_runtime_input_pending_event_t),
            _Alignof(h2_runtime_input_pending_event_t),
            &layout.allocation_size,
            &layout.input_pending_events_offset) ||
        !append_private_storage(
            layout.input_source_capacity,
            sizeof(h2_runtime_state_entry_t),
            _Alignof(h2_runtime_state_entry_t),
            &layout.allocation_size,
            &layout.state_entries_offset[0]) ||
        !append_private_storage(
            layout.input_source_capacity,
            sizeof(h2_runtime_state_entry_t),
            _Alignof(h2_runtime_state_entry_t),
            &layout.allocation_size,
            &layout.state_entries_offset[1]) ||
        !append_private_storage(
            layout.input_source_capacity,
            sizeof(h2_runtime_state_entry_t),
            _Alignof(h2_runtime_state_entry_t),
            &layout.allocation_size,
            &layout.state_entries_offset[2])) {
        return 0;
    }
    *out_layout = layout;
    return 1;
}

static void bind_private_storage(
    h2_runtime_private_t *private_state,
    const h2_runtime_private_layout_t *layout) {
    uint8_t *storage = (uint8_t *)private_state;
    private_state->allocation_size = layout->allocation_size;
    private_state->component_mapping_capacity =
        layout->component_mapping_capacity;
    private_state->component_mappings =
        (h2_runtime_component_mapping_t *)(storage +
                                           layout->component_mappings_offset);
    private_state->input_source_capacity = layout->input_source_capacity;
    private_state->input_sources =
        (h2_runtime_input_source_t *)(storage + layout->input_sources_offset);
    private_state->input_pending_event_capacity =
        layout->input_pending_event_capacity;
    private_state->input_pending_events =
        (h2_runtime_input_pending_event_t *)(
            storage + layout->input_pending_events_offset);
    private_state->event_payload_capacity = layout->event_payload_capacity;
    for (size_t index = 0u;
         index < H2_RUNTIME_STATE_SLOT_COUNT;
         ++index) {
        private_state->state_publication.banks[index].entries =
            (h2_runtime_state_entry_t *)(
                storage + layout->state_entries_offset[index]);
        private_state->state_publication.banks[index].entry_capacity =
            layout->input_source_capacity;
    }
}

static int runtime_video_decoder_is_complete(
    const h2_pal_video_decoder_api_t *decoder) {
    return decoder != NULL && decoder->vtable != NULL &&
        decoder->vtable->open != NULL && decoder->vtable->configure != NULL &&
        decoder->vtable->submit_packet != NULL &&
        decoder->vtable->acquire_frame != NULL &&
        decoder->vtable->frame_get_info != NULL &&
        decoder->vtable->release_frame != NULL && decoder->vtable->reset != NULL &&
        decoder->vtable->close != NULL;
}

static int runtime_audio_decoder_is_complete(
    const h2_pal_audio_decoder_api_t *decoder) {
    return decoder != NULL && decoder->vtable != NULL &&
        decoder->vtable->open != NULL && decoder->vtable->configure != NULL &&
        decoder->vtable->submit_packet != NULL &&
        decoder->vtable->acquire_frame != NULL &&
        decoder->vtable->frame_get_info != NULL &&
        decoder->vtable->release_frame != NULL && decoder->vtable->reset != NULL &&
        decoder->vtable->close != NULL;
}

h2_runtime_sequence_t h2_runtime_next_sequence(h2_runtime_t *runtime) {
    if (!h2_runtime_ready(runtime)) {
        return 0u;
    }
    while (atomic_flag_test_and_set_explicit(
        &runtime->private_state->sequence_lock,
        memory_order_acquire)) {
    }
    h2_runtime_sequence_t sequence = runtime->private_state->next_sequence++;
    if (sequence == 0u) {
        sequence = runtime->private_state->next_sequence++;
    }
    atomic_flag_clear_explicit(
        &runtime->private_state->sequence_lock,
        memory_order_release);
    return sequence;
}

static int runtime_config_is_valid(const h2_runtime_config_t *config) {
    if (config == NULL || config->board == NULL || config->target == NULL ||
        config->chip == NULL) {
        return 0;
    }
#define H2_RUNTIME_CONFIG_HAS_API(field) \
    (config->field != NULL)
    const int has_complete_surface =
        H2_RUNTIME_CONFIG_HAS_API(firmware_info) &&
        H2_RUNTIME_CONFIG_HAS_API(mem) && H2_RUNTIME_CONFIG_HAS_API(log) &&
        H2_RUNTIME_CONFIG_HAS_API(time) && H2_RUNTIME_CONFIG_HAS_API(timer) &&
        H2_RUNTIME_CONFIG_HAS_API(task) && H2_RUNTIME_CONFIG_HAS_API(queue) &&
        H2_RUNTIME_CONFIG_HAS_API(sync) && H2_RUNTIME_CONFIG_HAS_API(fs) &&
        H2_RUNTIME_CONFIG_HAS_API(disk) && H2_RUNTIME_CONFIG_HAS_API(pref) &&
        H2_RUNTIME_CONFIG_HAS_API(crypto) && H2_RUNTIME_CONFIG_HAS_API(http) &&
        H2_RUNTIME_CONFIG_HAS_API(net) && H2_RUNTIME_CONFIG_HAS_API(netif) &&
        H2_RUNTIME_CONFIG_HAS_API(mqtt) && H2_RUNTIME_CONFIG_HAS_API(webrtc) &&
        H2_RUNTIME_CONFIG_HAS_API(wifi_sta) && H2_RUNTIME_CONFIG_HAS_API(wifi_ap) &&
        H2_RUNTIME_CONFIG_HAS_API(wifi_csi) &&
        H2_RUNTIME_CONFIG_HAS_API(wifi_settings) && H2_RUNTIME_CONFIG_HAS_API(ble_host) &&
        H2_RUNTIME_CONFIG_HAS_API(modem) && H2_RUNTIME_CONFIG_HAS_API(power) &&
        H2_RUNTIME_CONFIG_HAS_API(display) && H2_RUNTIME_CONFIG_HAS_API(audio) &&
        runtime_audio_decoder_is_complete(config->audio_decoder) &&
        H2_RUNTIME_CONFIG_HAS_API(periph) && H2_RUNTIME_CONFIG_HAS_API(button) &&
        H2_RUNTIME_CONFIG_HAS_API(touch) &&
        H2_RUNTIME_CONFIG_HAS_API(buzzer) && H2_RUNTIME_CONFIG_HAS_API(nfc) &&
        H2_RUNTIME_CONFIG_HAS_API(imu) &&
        H2_RUNTIME_CONFIG_HAS_API(nfc_card_emulation) &&
        H2_RUNTIME_CONFIG_HAS_API(gpio_irq) && H2_RUNTIME_CONFIG_HAS_API(led) &&
        H2_RUNTIME_CONFIG_HAS_API(switch_api) && H2_RUNTIME_CONFIG_HAS_API(pwm_switch) &&
        H2_RUNTIME_CONFIG_HAS_API(input) && H2_RUNTIME_CONFIG_HAS_API(system_event) &&
        runtime_video_decoder_is_complete(config->video_decoder);
#undef H2_RUNTIME_CONFIG_HAS_API
    return has_complete_surface;
}

static h2_pal_result_t runtime_init_release(
    const h2_runtime_config_t *config,
    h2_runtime_t *runtime,
    h2_pal_result_t result) {
    if (runtime != NULL) {
        h2_runtime_private_t *private_state = runtime->private_state;
        if (private_state != NULL) {
            /* Init never starts the poller, so only prepared state is here. */
            h2_runtime_input_release(runtime);
            h2_runtime_stop_system_events(runtime);
            if (private_state->event_queue != NULL) {
                h2_pal_queue_destroy(
                    runtime->queue, private_state->event_queue);
                private_state->event_queue = NULL;
            }
            if (private_state->input_push_edge_queue != NULL) {
                h2_pal_queue_destroy(
                    runtime->queue, private_state->input_push_edge_queue);
                private_state->input_push_edge_queue = NULL;
            }
            if (private_state->input_nfc_result_queue != NULL) {
                h2_pal_queue_destroy(
                    runtime->queue, private_state->input_nfc_result_queue);
                private_state->input_nfc_result_queue = NULL;
            }
            size_t allocation_size = private_state->allocation_size;
            memset(private_state, 0, allocation_size);
            h2_pal_mem_free(config->mem, private_state);
        }
        memset(runtime, 0, sizeof(*runtime));
        h2_pal_mem_free(config->mem, runtime);
    }
    return result;
}

h2_pal_result_t h2_runtime_init(
    const h2_runtime_config_t *config,
    h2_runtime_t **out_runtime) {
    if (out_runtime == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_runtime = NULL;

    if (!runtime_config_is_valid(config)) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    h2_runtime_private_layout_t private_layout;
    if (!runtime_private_layout(config, &private_layout)) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    h2_runtime_t *runtime =
        (h2_runtime_t *)h2_pal_mem_alloc(config->mem, sizeof(*runtime));
    if (runtime == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(runtime, 0, sizeof(*runtime));

    h2_runtime_private_t *private_state =
        (h2_runtime_private_t *)h2_pal_mem_alloc(
            config->mem,
            private_layout.allocation_size);
    if (private_state == NULL) {
        return runtime_init_release(
            config, runtime, H2_PAL_ERR_NO_MEMORY);
    }
    memset(private_state, 0, private_layout.allocation_size);
    bind_private_storage(private_state, &private_layout);

    runtime->board = config->board;
    runtime->target = config->target;
    runtime->chip = config->chip;
    runtime->private_state = private_state;

#define H2_RUNTIME_BIND_PROXY(field)                                                \
    do {                                                                            \
        if (config->field != NULL) {                                                 \
            private_state->field##_proxy = *config->field;                           \
            runtime->field = &private_state->field##_proxy;                          \
        }                                                                            \
    } while (0)

    H2_RUNTIME_BIND_PROXY(firmware_info);
    H2_RUNTIME_BIND_PROXY(mem);
    H2_RUNTIME_BIND_PROXY(log);
    H2_RUNTIME_BIND_PROXY(time);
    H2_RUNTIME_BIND_PROXY(timer);
    H2_RUNTIME_BIND_PROXY(task);
    H2_RUNTIME_BIND_PROXY(queue);
    H2_RUNTIME_BIND_PROXY(sync);
    H2_RUNTIME_BIND_PROXY(fs);
    H2_RUNTIME_BIND_PROXY(disk);
    H2_RUNTIME_BIND_PROXY(pref);
    H2_RUNTIME_BIND_PROXY(crypto);
    H2_RUNTIME_BIND_PROXY(http);
    H2_RUNTIME_BIND_PROXY(net);
    H2_RUNTIME_BIND_PROXY(netif);
    H2_RUNTIME_BIND_PROXY(mqtt);
    H2_RUNTIME_BIND_PROXY(webrtc);
    runtime->webrtc_media_track = config->webrtc_media_track;
    H2_RUNTIME_BIND_PROXY(wifi_sta);
    H2_RUNTIME_BIND_PROXY(wifi_ap);
    H2_RUNTIME_BIND_PROXY(wifi_csi);
    H2_RUNTIME_BIND_PROXY(wifi_settings);
    H2_RUNTIME_BIND_PROXY(ble_host);
    H2_RUNTIME_BIND_PROXY(modem);
    H2_RUNTIME_BIND_PROXY(power);
    H2_RUNTIME_BIND_PROXY(display);
    H2_RUNTIME_BIND_PROXY(audio);
    H2_RUNTIME_BIND_PROXY(audio_decoder);
    H2_RUNTIME_BIND_PROXY(periph);
    H2_RUNTIME_BIND_PROXY(button);
    H2_RUNTIME_BIND_PROXY(touch);
    H2_RUNTIME_BIND_PROXY(buzzer);
    H2_RUNTIME_BIND_PROXY(nfc);
    H2_RUNTIME_BIND_PROXY(nfc_card_emulation);
    H2_RUNTIME_BIND_PROXY(imu);
    H2_RUNTIME_BIND_PROXY(gpio_irq);
    H2_RUNTIME_BIND_PROXY(led);
    H2_RUNTIME_BIND_PROXY(input);
    H2_RUNTIME_BIND_PROXY(system_event);
    H2_RUNTIME_BIND_PROXY(video_decoder);
    if (config->switch_api != NULL) {
        private_state->switch_proxy = *config->switch_api;
        runtime->switch_api = &private_state->switch_proxy;
    }
    H2_RUNTIME_BIND_PROXY(pwm_switch);

#undef H2_RUNTIME_BIND_PROXY

    private_state->initialized = 1;
    atomic_flag_clear(&private_state->sequence_lock);
    atomic_init(&private_state->system_event_active, 0);
    atomic_init(
        &private_state->input_phase,
        H2_RUNTIME_INPUT_PHASE_STOPPED);
    atomic_init(&private_state->input_stop_requested, 0);
    atomic_init(&private_state->input_worker_result, H2_PAL_OK);
    private_state->next_sequence = 1u;
    private_state->input_sources_ready = 0;

    size_t event_queue_capacity = config->event_queue_capacity;
    if (event_queue_capacity == 0u) {
        event_queue_capacity = H2_RUNTIME_DEFAULT_EVENT_QUEUE_CAPACITY;
    }

    const h2_pal_queue_config_t event_queue_config = {
        .name = "h2-runtime-events",
        .item_size = offsetof(h2_runtime_queued_event_t, payload) +
            private_state->event_payload_capacity,
        .item_count = event_queue_capacity,
        .allocator = config->mem,
    };
    h2_pal_result_t rc =
        h2_pal_queue_create(runtime->queue, &event_queue_config, &runtime->private_state->event_queue);
    if (rc != H2_PAL_OK) {
        return runtime_init_release(config, runtime, rc);
    }

    const h2_pal_queue_config_t push_edge_queue_config = {
        .name = "h2-runtime-button-push-edges",
        .item_size = sizeof(h2_runtime_button_push_edge_t),
        .item_count = H2_RUNTIME_BUTTON_PUSH_EDGE_QUEUE_CAPACITY,
        .allocator = config->mem,
    };
    rc = h2_pal_queue_create(
        runtime->queue,
        &push_edge_queue_config,
        &runtime->private_state->input_push_edge_queue);
    if (rc != H2_PAL_OK) {
        return runtime_init_release(config, runtime, rc);
    }

    const h2_pal_queue_config_t nfc_result_queue_config = {
        .name = "h2-runtime-nfc-results",
        .item_size = sizeof(h2_runtime_input_nfc_result_t),
        .item_count = private_state->input_source_capacity,
        .allocator = config->mem,
    };
    rc = h2_pal_queue_create(
        runtime->queue,
        &nfc_result_queue_config,
        &runtime->private_state->input_nfc_result_queue);
    if (rc != H2_PAL_OK) {
        return runtime_init_release(config, runtime, rc);
    }

    rc = h2_runtime_start_system_events(runtime);
    if (rc != H2_PAL_OK) {
        return runtime_init_release(config, runtime, rc);
    }

    rc = h2_runtime_build_component_mappings(runtime, config->component_mapper);
    if (rc != H2_PAL_OK) {
        return runtime_init_release(config, runtime, rc);
    }

    h2_runtime_state_set_publish_interval(
        runtime, config->state.publish_interval_ms);

    /*
     * Build the input writer mutex, the state publication and the input
     * source table. All three are derived from data that is fixed for the
     * life of the Runtime, so they are created once here and survive every
     * poller start/stop cycle.
     */
    rc = h2_runtime_input_prepare(runtime);
    if (rc != H2_PAL_OK) {
        return runtime_init_release(config, runtime, rc);
    }

    /*
     * The poller itself is not started here. h2_runtime_input_start() and
     * h2_runtime_input_stop() are public and caller-owned: the launcher
     * starts acquisition once component validation has passed, and may stop
     * and start it again while the Runtime stays initialized.
     */
    *out_runtime = runtime;
    return H2_PAL_OK;
}

h2_pal_result_t h2_runtime_periph_id(
    const h2_runtime_t *runtime,
    h2_runtime_component_id_t component_id,
    h2_pal_periph_id_t *out_periph_id) {
    if (!h2_runtime_ready(runtime) || component_id == H2_RUNTIME_COMPONENT_ID_NONE ||
        out_periph_id == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    for (size_t i = 0u; i < runtime->private_state->component_mapping_count; ++i) {
        const h2_runtime_component_mapping_t *mapping =
            &runtime->private_state->component_mappings[i];
        if (mapping->component_id == component_id) {
            if (mapping->periph_id == 0u) {
                return H2_PAL_ERR_UNSUPPORTED;
            }
            *out_periph_id = mapping->periph_id;
            return H2_PAL_OK;
        }
    }
    return H2_PAL_ERR_NOT_FOUND;
}

void h2_runtime_deinit(h2_runtime_t *runtime) {
    if (runtime == NULL) {
        return;
    }

    if (runtime->private_state->test_control != NULL) {
        return;
    }

    h2_pal_result_t input_stop_rc = h2_runtime_input_stop(runtime);
    if (input_stop_rc != H2_PAL_OK &&
        runtime->private_state->input_task != NULL) {
        return;
    }
    h2_runtime_input_release(runtime);
    h2_runtime_stop_system_events(runtime);
    /* Last producer to shut down: app tasks posting custom events. */
    h2_runtime_custom_event_close(runtime);

    if (runtime->private_state->event_queue != NULL) {
        h2_pal_queue_destroy(runtime->queue, runtime->private_state->event_queue);
        runtime->private_state->event_queue = NULL;
    }
    if (runtime->private_state->input_push_edge_queue != NULL) {
        h2_pal_queue_destroy(
            runtime->queue,
            runtime->private_state->input_push_edge_queue);
        runtime->private_state->input_push_edge_queue = NULL;
    }
    if (runtime->private_state->input_nfc_result_queue != NULL) {
        h2_pal_queue_destroy(
            runtime->queue, runtime->private_state->input_nfc_result_queue);
        runtime->private_state->input_nfc_result_queue = NULL;
    }

    const h2_pal_mem_api_t mem = *runtime->mem;
    h2_runtime_private_t *private_state = runtime->private_state;
    size_t allocation_size = private_state->allocation_size;
    memset(private_state, 0, allocation_size);
    h2_pal_mem_free(&mem, private_state);
    memset(runtime, 0, sizeof(*runtime));
    h2_pal_mem_free(&mem, runtime);
}
