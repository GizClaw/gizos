#ifndef H2_JIELI_WL82_PLATFORM_IMAGE_H
#define H2_JIELI_WL82_PLATFORM_IMAGE_H

#include "hal/h2_pal_power.h"
#include "os/h2_pal_fs.h"
#include "os/h2_pal_image.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Borrowed for the firmware lifetime. Both banks execute through the same
 * remapped CPU window; partition IDs describe physical storage, not roles.
 * Shadows contain the exact UFW input, never a raw NOR-bank representation. */
typedef struct h2_jieli_wl82_image_config {
  const h2_pal_fs_api_t* fs;
  uint32_t bank_1_partition_id;
  uint32_t bank_2_partition_id;
  uint32_t image_capacity;
  const char* shadow_temp_path;
  const char* bank_1_shadow_path;
  const char* bank_2_shadow_path;
} h2_jieli_wl82_image_config_t;

/* Idempotent for the same borrowed config. Creates boot-lifetime callback and
 * semaphore storage once; even failed/late native completions cannot outlive
 * it. Call after FS initialization, from a worker. */
h2_pal_result_t h2_jieli_wl82_platform_image_configure(
    const h2_jieli_wl82_image_config_t* config);
const h2_pal_image_api_t* h2_jieli_wl82_platform_image_api(void);
const h2_pal_power_api_t* h2_jieli_wl82_platform_power_api(void);

/* Native evidence ABI: format 1/40 bytes is deferred boot publication evidence
 * (LE code length, LE CRC16, two zero bytes, encrypted 32-byte header).
 * prepare(EXISTING, running) is a read-only observation returning format 2,
 * the exact native BootInfo object bytes. It never consumes update state. That
 * observation is suitable only as opaque bootstrap-identity input, not as the
 * exact STORED artifact hash or as deferred publication evidence. */

#ifdef __cplusplus
}
#endif
#endif
