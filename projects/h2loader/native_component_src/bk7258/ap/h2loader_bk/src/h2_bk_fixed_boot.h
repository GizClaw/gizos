#ifndef H2_BK_FIXED_BOOT_H
#define H2_BK_FIXED_BOOT_H
#include "layout.h"
#include <stdint.h>
/* Windows derived from this image's partition table, or NULL when the board
 * layout is not a fixed Loader/App layout. */
const h2_fixed_layout_t* h2_bk_fixed_layout(void);
int h2_bk_fixed_layout_active(void);
/* 1 = verified flashed mapped_ab_v1, 0 = stock ABI, negative = read/format
 * error. No cached descriptor: verification must precede each Loader write. */
int h2_bk_fixed_mapped_abi(void);
/* 1 = mapped ABI with canonical A image size, 0 = legacy AES-off, error
 * otherwise. Read-only; no boot flag changes, erases or writes. */
int h2_bk_fixed_loader_update_check(uint64_t image_size);
/* OK when selection can resolve a role: stock ABI, or mapped ABI with a bound
 * boot context. Read-only, so writers can reject before any erase. */
int h2_bk_fixed_boot_context_check(void);
uint8_t h2_bk_fixed_current_slot(void);
int h2_bk_fixed_select(uint32_t partition_id);
int h2_bk_fixed_next_app(void);
int h2_bk_fixed_app_failed(void);
int h2_bk_fixed_confirm_app(void);
int h2_bk_fixed_invalidate_app(void);
/* 1 = Loader, 0 = App/erased, negative = read/format failure. */
int h2_bk_fixed_app_window_holds_loader(void);
int h2_bk_fixed_confirm_loader(void);
/* OK only while executing native B with B confirmed; read-only. */
int h2_bk_fixed_relay_confirmed(void);
int h2_bk_fixed_relay_failed(void);
/* Copy a staged Loader image's RBL head area to the end of the App window. */
int h2_bk_fixed_publish_relay_head(uint32_t window_offset, uint32_t window_size,
                                   uint32_t image_size, uint32_t head_size);
#endif
