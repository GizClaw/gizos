#ifndef H2_BK_COREDUMP_H
#define H2_BK_COREDUMP_H

#include "h2/pal/core/h2_pal_errors.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_bk_coredump_info {
  uint32_t processor; /* 1: AP, 2: CP. */
  uint32_t core;
  uint32_t reason;
  uint32_t pc;
  uint32_t lr;
  uint32_t sp;
  uint32_t cfsr;
  uint32_t hfsr;
  uint32_t stack_address;
  uint32_t stack_bytes;
} h2_bk_coredump_info_t;

/** Configure an existing, sector-aligned coredump partition after flash init.
 * Call while the application is quiescent. No erase is performed. Nonblank
 * storage is preserved and leaves capture disarmed until the next boot after
 * an explicit erase. Native hooks capture after SDK exception coordination;
 * later local faults cannot overwrite the record.
 * Current stack capture covers internal SRAM/DTCM only.
 * Disabled builds return UNSUPPORTED. */
int h2_bk_coredump_configure(uint32_t address, uint32_t size);
bool h2_bk_coredump_armed(void);
/** Read and validate the persisted register record and optional stack record.
 * Normal task context only. NOT_FOUND means no valid register record. */
int h2_bk_coredump_read_info(h2_bk_coredump_info_t* out_info);

#ifdef __cplusplus
}
#endif

#endif
