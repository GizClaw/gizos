#include "h2_runtime_internal.h"

#include <string.h>

typedef struct h2_runtime_mapping_build_context {
    h2_runtime_t *runtime;
    h2_runtime_component_t component;
} h2_runtime_mapping_build_context_t;

h2_runtime_component_t h2_runtime_component_from_periph_type(
    h2_pal_periph_type_t type) {
    switch (type) {
    case H2_PAL_PERIPH_TYPE_SINGLE_BUTTON:
    case H2_PAL_PERIPH_TYPE_RADIO_BUTTON:
        return H2_RUNTIME_COMPONENT_BUTTON;
    case H2_PAL_PERIPH_TYPE_NFC_READER:
        return H2_RUNTIME_COMPONENT_NFC_READER;
    case H2_PAL_PERIPH_TYPE_IMU:
        return H2_RUNTIME_COMPONENT_IMU;
    case H2_PAL_PERIPH_TYPE_BATTERY:
        return H2_RUNTIME_COMPONENT_BATTERY;
    case H2_PAL_PERIPH_TYPE_PWM_SWITCH:
        return H2_RUNTIME_COMPONENT_PWM_SWITCH;
    case H2_PAL_PERIPH_TYPE_LED_STRIP:
        return H2_RUNTIME_COMPONENT_LED_STRIP;
    case H2_PAL_PERIPH_TYPE_TEMPERATURE_SENSOR:
        return H2_RUNTIME_COMPONENT_TEMPERATURE_SENSOR;
    case H2_PAL_PERIPH_TYPE_BUZZER:
        return H2_RUNTIME_COMPONENT_BUZZER;
    default:
        return H2_RUNTIME_COMPONENT_NONE;
    }
}

static h2_runtime_component_t mapping_component_from_periph_type(
    h2_pal_periph_type_t type) {
    if (type == H2_PAL_PERIPH_TYPE_GPIO_IRQ) {
        return H2_RUNTIME_COMPONENT_SYSTEM_GPIO_IRQ;
    }
    return h2_runtime_component_from_periph_type(type);
}

static h2_pal_result_t collect_mapping(
    void *user,
    const h2_runtime_component_mapping_entry_t *entry) {
    h2_runtime_mapping_build_context_t *context =
        (h2_runtime_mapping_build_context_t *)user;
    if (context == NULL || entry == NULL ||
        entry->component_id == H2_RUNTIME_COMPONENT_ID_NONE ||
        entry->periph_id == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    h2_pal_periph_info_t peripheral = {0};
    h2_pal_result_t rc = h2_pal_periph_get(
        context->runtime->periph, entry->periph_id, &peripheral);
    if (rc == H2_PAL_ERR_NOT_FOUND) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (peripheral.id != entry->periph_id ||
        mapping_component_from_periph_type(peripheral.type) !=
            context->component) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    h2_runtime_private_t *private_state = context->runtime->private_state;
    for (size_t i = 0u; i < private_state->component_mapping_count; ++i) {
        const h2_runtime_component_mapping_t *mapping =
            &private_state->component_mappings[i];
        if (mapping->component_id == entry->component_id ||
            mapping->periph_id == entry->periph_id) {
            return H2_PAL_ERR_INVALID_ARG;
        }
    }
    if (private_state->component_mapping_count >=
        private_state->component_mapping_capacity) {
        return H2_PAL_ERR_NO_SPACE;
    }

    private_state->component_mappings[private_state->component_mapping_count++] =
        (h2_runtime_component_mapping_t){
            .component = context->component,
            .component_id = entry->component_id,
            .periph_id = entry->periph_id,
        };
    return H2_PAL_OK;
}

h2_pal_result_t h2_runtime_build_component_mappings(
    h2_runtime_t *runtime,
    const h2_runtime_component_mapper_t *mapper) {
    if (!h2_runtime_ready(runtime)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(
        runtime->private_state->component_mappings,
        0,
        runtime->private_state->component_mapping_capacity *
            sizeof(*runtime->private_state->component_mappings));
    runtime->private_state->component_mapping_count = 0u;
    if (mapper == NULL) {
        return H2_PAL_OK;
    }
    if (runtime->periph == NULL || mapper->vtable == NULL || mapper->vtable->list == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    h2_runtime_mapping_build_context_t context = {
        .runtime = runtime,
    };
    h2_pal_result_t rc = H2_PAL_OK;

    static const h2_runtime_component_t components[] = {
        H2_RUNTIME_COMPONENT_SYSTEM_GPIO_IRQ,
        H2_RUNTIME_COMPONENT_BUTTON,
        H2_RUNTIME_COMPONENT_NFC_READER,
        H2_RUNTIME_COMPONENT_IMU,
        H2_RUNTIME_COMPONENT_BATTERY,
        H2_RUNTIME_COMPONENT_PWM_SWITCH,
        H2_RUNTIME_COMPONENT_LED_STRIP,
        H2_RUNTIME_COMPONENT_TEMPERATURE_SENSOR,
        H2_RUNTIME_COMPONENT_BUZZER,
    };
    for (size_t i = 0u; i < sizeof(components) / sizeof(components[0]); ++i) {
        context.component = components[i];
        rc = mapper->vtable->list(
            mapper->user,
            context.component,
            collect_mapping,
            &context);
        if (rc != H2_PAL_OK) {
            memset(
                runtime->private_state->component_mappings,
                0,
                runtime->private_state->component_mapping_capacity *
                    sizeof(*runtime->private_state->component_mappings));
            runtime->private_state->component_mapping_count = 0u;
            return rc;
        }
    }
    return H2_PAL_OK;
}

h2_pal_result_t h2_runtime_component_get(
    const h2_runtime_t *runtime,
    h2_runtime_component_id_t component_id,
    h2_runtime_component_info_t *out_info) {
    if (!h2_runtime_ready(runtime) ||
        component_id == H2_RUNTIME_COMPONENT_ID_NONE || out_info == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    for (size_t i = 0u; i < runtime->private_state->component_mapping_count;
         ++i) {
        const h2_runtime_component_mapping_t *mapping =
            &runtime->private_state->component_mappings[i];
        if (mapping->component_id == component_id) {
            *out_info = (h2_runtime_component_info_t){
                .component_id = mapping->component_id,
                .kind = mapping->component,
            };
            return H2_PAL_OK;
        }
    }
    return H2_PAL_ERR_NOT_FOUND;
}

const h2_runtime_component_mapping_t *h2_runtime_find_component_mapping_by_periph(
    const h2_runtime_t *runtime,
    h2_pal_periph_id_t periph_id) {
    if (!h2_runtime_ready(runtime) || periph_id == 0u) {
        return NULL;
    }
    for (size_t i = 0u; i < runtime->private_state->component_mapping_count; ++i) {
        const h2_runtime_component_mapping_t *mapping =
            &runtime->private_state->component_mappings[i];
        if (mapping->periph_id == periph_id) {
            return mapping;
        }
    }
    return NULL;
}

h2_runtime_input_source_t *h2_runtime_find_input_source(
    h2_runtime_t *runtime,
    h2_runtime_component_t component,
    h2_runtime_component_id_t component_id) {
    if (!h2_runtime_ready(runtime) || component_id == H2_RUNTIME_COMPONENT_ID_NONE) {
        return NULL;
    }
    for (size_t i = 0u; i < runtime->private_state->input_source_count; ++i) {
        h2_runtime_input_source_t *source = &runtime->private_state->input_sources[i];
        if (source->component == component && source->component_id == component_id) {
            return source;
        }
    }
    return NULL;
}

const h2_runtime_input_source_t *h2_runtime_find_input_source_const(
    const h2_runtime_t *runtime,
    h2_runtime_component_t component,
    h2_runtime_component_id_t component_id) {
    return h2_runtime_find_input_source((h2_runtime_t *)runtime, component, component_id);
}
