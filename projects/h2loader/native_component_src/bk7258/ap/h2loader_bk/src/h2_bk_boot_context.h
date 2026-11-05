#ifndef H2_BK_BOOT_CONTEXT_H
#define H2_BK_BOOT_CONTEXT_H

#include <stdint.h>

/* Read committed Partition 2 metadata on every use; never cache Stage role.
 * Returns 1 for Loader, 0 for App, or a PAL error. */
int h2_bk_boot_candidate_is_loader(uint64_t* out_image_size);
/* OK when bound, INVALID_STATE otherwise. Checks no metadata. */
int h2_bk_boot_context_ready(void);
/* Read committed Partition 1 metadata. Core clears it before erasing A and
 * restores it after verifying A. Returns 1 for valid, 0 for missing or cleared,
 * or a PAL error. */
int h2_bk_boot_loader_window_committed(void);

#endif
