#ifndef H2_GIZCLAW_E2E_LEDGER_H
#define H2_GIZCLAW_E2E_LEDGER_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/os/h2_pal_mem.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_GIZCLAW_E2E_LEDGER_PAGE_BYTES 4096u

typedef struct h2_gizclaw_e2e_ledger_page h2_gizclaw_e2e_ledger_page_t;

/* The caller serializes append/freeze and keeps frozen bytes immutable during
 * replay. init borrows a contiguous buffer; init_paged borrows an allocator and
 * owns lazily allocated pages. Neither mode prints or performs I/O. */
typedef struct h2_gizclaw_e2e_ledger {
  char *data;
  size_t capacity, size, records;
  uint32_t crc32;
  int error;
  bool frozen;
  const h2_pal_mem_api_t *allocator;
  h2_gizclaw_e2e_ledger_page_t *first, *last;
} h2_gizclaw_e2e_ledger_t;

int h2_gizclaw_e2e_ledger_init(h2_gizclaw_e2e_ledger_t *ledger, char *buffer,
                              size_t capacity);
int h2_gizclaw_e2e_ledger_init_paged(h2_gizclaw_e2e_ledger_t *ledger,
                                     const h2_pal_mem_api_t *allocator,
                                     size_t capacity);
void h2_gizclaw_e2e_ledger_append(h2_gizclaw_e2e_ledger_t *ledger,
                                 const char *record, size_t size);
int h2_gizclaw_e2e_ledger_freeze(h2_gizclaw_e2e_ledger_t *ledger);
/* Return a frozen contiguous span at offset, without copying. At size, return
 * an empty span. Advance by out_size to replay both storage modes identically.
 */
int h2_gizclaw_e2e_ledger_span(const h2_gizclaw_e2e_ledger_t *ledger,
                               size_t offset, const char **out_data,
                               size_t *out_size);
/* Call only after producers and readers have quiesced, and before re-init.
 * Borrowed storage is never freed. Owned pages are released even after error.
 */
void h2_gizclaw_e2e_ledger_deinit(h2_gizclaw_e2e_ledger_t *ledger);

#ifdef __cplusplus
}
#endif

#endif
