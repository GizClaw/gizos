#ifndef H2_BK7258_BOARD_STORAGE_H
#define H2_BK7258_BOARD_STORAGE_H
#include "h2_bk_platform_storage.h"
#define H2_BK7258_COREDUMP_ADDR 0x7a0000u
#define H2_BK7258_COREDUMP_SIZE (360u * 1024u)
static inline int h2_bk7258_board_disk_configure(void) {
  const h2_bk_disk_region_t region = {
      .id = H2_BK7258_COREDUMP_ADDR,
      .offset = H2_BK7258_COREDUMP_ADDR,
      .size = H2_BK7258_COREDUMP_SIZE,
      .name = "coredump",
  };
  return h2_bk_platform_disk_configure(&region);
}
#endif
