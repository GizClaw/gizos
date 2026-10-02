#ifndef H2_TEST_PREF_PORT_FLASHDB_H
#define H2_TEST_PREF_PORT_FLASHDB_H
#include <stddef.h>
#include <stdint.h>
#define FLASHDB_DEV_NAME "flashdb0"
struct fal_flash_dev {
  char name[24];
  uint32_t addr;
  size_t len;
  size_t blk_size;
  struct {
    int (*init)(void);
    int (*read)(long offset, uint8_t *buffer, size_t size);
    int (*write)(long offset, const uint8_t *buffer, size_t size);
    int (*erase)(long offset, size_t size);
  } ops;
  size_t write_gran;
};
extern struct fal_flash_dev g_flashdb0;
#endif
