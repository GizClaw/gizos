#ifndef H2_BK_FIXED_BOOT_H
#define H2_BK_FIXED_BOOT_H
#include <stdint.h>
int h2_bk_fixed_layout_active(void);
uint8_t h2_bk_fixed_current_slot(void);
int h2_bk_fixed_select(uint32_t partition_id);
int h2_bk_fixed_next_app(void);
int h2_bk_fixed_app_failed(void);
int h2_bk_fixed_confirm_app(void);
int h2_bk_fixed_invalidate_app(void);
#endif
