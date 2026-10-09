#ifndef H2_CHIP_BOARD_H
#define H2_CHIP_BOARD_H

#include "h2_mosaico_revision.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Return the static board identity; the caller must not free it. */
const char *h2_chip_board_name(void);

/** Read and validate eFuse hardware revision without driving any GPIO.
 * Call from task context. Returns 0 on success, -1 on invalid output, SDK
 * read failure or unsupported/unprogrammed hardware. Clears output on failure.
 * No state is retained; repeat calls are allowed.
 */
int h2_mosaico_board_revision(h2_mosaico_revision_t *out_revision);

#ifdef __cplusplus
}
#endif
#endif
