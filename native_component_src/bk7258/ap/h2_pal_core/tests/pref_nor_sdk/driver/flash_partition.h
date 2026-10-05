#ifndef H2_PREF_NOR_PARTITION_H
#define H2_PREF_NOR_PARTITION_H
#include <stdint.h>
#define BK_PARTITION_FLASHDB 1
typedef struct { uint32_t partition_start_addr,partition_length; } bk_logic_partition_t;
const bk_logic_partition_t *bk_flash_partition_get_info(int);
#endif
