#ifndef H2_BK_FIXED_BOOT_H
#define H2_BK_FIXED_BOOT_H
#include "layout.h"
#include <stdint.h>
/* Windows derived from this image's partition table, or NULL when the board
 * layout is not a fixed Loader/App layout. */
const h2_fixed_layout_t *h2_bk_fixed_layout(void);
int h2_bk_fixed_layout_active(void);
uint8_t h2_bk_fixed_current_slot(void);
int h2_bk_fixed_select(uint32_t partition_id);
int h2_bk_fixed_next_app(void);
int h2_bk_fixed_app_failed(void);
int h2_bk_fixed_confirm_app(void);
int h2_bk_fixed_invalidate_app(void);
int h2_bk_fixed_app_window_holds_loader(void);
int h2_bk_fixed_confirm_loader(void);
#endif
