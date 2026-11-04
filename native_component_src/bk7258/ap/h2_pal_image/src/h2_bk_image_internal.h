#ifndef H2_BK_IMAGE_INTERNAL_H
#define H2_BK_IMAGE_INTERNAL_H
#include <stdint.h>
/* Boot provider shares only native transaction facts with Image. */
int h2_bk_image_write_active(void);
int h2_bk_image_prepared(uint32_t partition_id);
int h2_bk_image_publish_native_footer(uint32_t partition_id);
void h2_bk_image_selected(uint32_t partition_id);
int h2_bk_image_execution_partition(uint32_t partition_id,
                                    uint32_t* out_execution_partition);
#endif
