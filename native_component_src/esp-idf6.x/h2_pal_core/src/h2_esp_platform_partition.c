#include "h2_esp_platform_core.h"
#include "h2_esp_platform_safe_call.h"

#include "esp_attr.h"
#include "esp_partition.h"

#include <string.h>

#define H2_ESP_PARTITION_LABEL_CAPACITY 17u
#define H2_ESP_PARTITION_SAFE_STACK_DEPTH 4096u

typedef struct h2_esp_partition_lookup {
    char label[H2_ESP_PARTITION_LABEL_CAPACITY];
    uint8_t subtype;
    h2_pal_result_t result;
} h2_esp_partition_lookup_t;

static void IRAM_ATTR partition_lookup_safe_callback(void *context) {
    h2_esp_partition_lookup_t *lookup =
        (h2_esp_partition_lookup_t *)context;
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_ANY,
        lookup->label);

    if (partition == NULL) {
        lookup->result = H2_PAL_ERR_NOT_FOUND;
        return;
    }
    lookup->subtype = (uint8_t)partition->subtype;
    lookup->result = H2_PAL_OK;
}

h2_pal_result_t h2_esp_platform_data_partition_subtype(
    const char *label,
    uint8_t *out_subtype) {
    h2_esp_partition_lookup_t lookup = {0};
    size_t label_len;
    h2_pal_result_t rc;

    if (label == NULL || out_subtype == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_subtype = 0u;
    label_len = strnlen(label, sizeof(lookup.label));
    if (label_len == 0u || label_len >= sizeof(lookup.label)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memcpy(lookup.label, label, label_len + 1u);
    rc = h2_esp_platform_safe_call(
        partition_lookup_safe_callback,
        &lookup,
        sizeof(lookup),
        H2_ESP_PARTITION_SAFE_STACK_DEPTH);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (lookup.result != H2_PAL_OK) {
        return lookup.result;
    }
    *out_subtype = lookup.subtype;
    return H2_PAL_OK;
}
