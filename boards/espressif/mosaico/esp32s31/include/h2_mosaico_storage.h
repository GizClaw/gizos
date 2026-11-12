#ifndef H2_MOSAICO_STORAGE_H
#define H2_MOSAICO_STORAGE_H

#include "core/h2_pal_errors.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Blocking, read-only storage queries for a worker, never the UI task.
 * Outputs are zeroed on failure. Internal usage includes firmware image bytes,
 * allocated LittleFS blocks, and reserved boot/partition/metadata regions;
 * total is physical internal flash capacity. NAND usage is mapped logical
 * bytes and total is usable FTL capacity, excluding spare/metadata blocks.
 * Queries do not mount, format, erase, sync, or write storage.
 * NAND is initialized on first query if needed, borrowing an existing BSP
 * handle otherwise. Serialize queries and deinit; exclude other BSP NAND
 * init/deinit operations for this borrow's lifetime.
 */
h2_pal_result_t h2_mosaico_storage_internal(uint64_t* used_bytes,
                                            uint64_t* total_bytes);
h2_pal_result_t h2_mosaico_storage_nand(uint64_t* used_bytes,
                                        uint64_t* total_bytes);

/** Releases only NAND initialized by these queries, after all queries join.
 * Failure retains ownership for retry. Existing borrowed handles remain open.
 */
h2_pal_result_t h2_mosaico_storage_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
