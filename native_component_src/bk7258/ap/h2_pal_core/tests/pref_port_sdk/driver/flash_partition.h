#ifndef H2_TEST_PREF_PORT_PARTITION_H
#define H2_TEST_PREF_PORT_PARTITION_H
#include <stdint.h>
#define BK_PARTITION_FLASHDB 1
typedef struct bk_logic_partition {
  uint32_t partition_start_addr;
  uint32_t partition_length;
} bk_logic_partition_t;
const bk_logic_partition_t *bk_flash_partition_get_info(int id);
#endif
