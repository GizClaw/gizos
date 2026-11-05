#ifndef H2_ESP_PLATFORM_IMAGE_H
#define H2_ESP_PLATFORM_IMAGE_H

#include "h2/pal/os/h2_pal_image.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Native ESP OTA artifacts. IDs 1 and 2 map to OTA0 and OTA1. Both views
 * expose esp_partition_read bytes, including the native image header/trailer.
 * NATIVE size is exactly esp_image_get_metadata.image_len. STORED size is the
 * complete accepted artifact (including any padding), known only for the
 * retained prepared transaction. It is unknown after reset/abort; no persistent
 * artifact-length receipt is inferred from native metadata.
 * ESP images can execute from either OTA slot; no link-window fact is exposed.
 * The provider retains no caller buffers and never selects a boot partition.
 */
const h2_pal_image_api_t* h2_esp_platform_image_api(void);

#ifdef __cplusplus
}
#endif

#endif
