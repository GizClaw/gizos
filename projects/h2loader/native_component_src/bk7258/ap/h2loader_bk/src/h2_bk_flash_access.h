#ifndef H2_BK_FLASH_ACCESS_H
#define H2_BK_FLASH_ACCESS_H

#include <stddef.h>
#include <stdint.h>

/* Task context only. Raw CRC is checked before each mapped data read.
 * Callers serialize against programming this window. Output is undefined on
 * failure. Erased packets return FF without a mapped load; vector callers
 * must reject erased/invalid stack and reset words. */
int h2_bk_flash_read_plain(uint32_t physical, uint32_t capacity,
                           uint32_t offset, void* output, size_t size);
int h2_bk_flash_access_ready(void);
/* One read-only query per boot; unknown/failure never permits Loader relay. */
unsigned h2_bk_flash_aes_state(void);

#endif
