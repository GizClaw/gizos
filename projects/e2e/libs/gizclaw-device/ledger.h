#ifndef H2_GIZCLAW_E2E_LEDGER_H
#define H2_GIZCLAW_E2E_LEDGER_H

#include "h2/pal/core/h2_pal_errors.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Borrowed storage. The caller serializes append/freeze and keeps frozen bytes
 * immutable until process teardown; this helper does not allocate or print. */
typedef struct h2_gizclaw_e2e_ledger {
  char *data;
  size_t capacity, size, records;
  uint32_t crc32;
  int error;
  bool frozen;
} h2_gizclaw_e2e_ledger_t;

int h2_gizclaw_e2e_ledger_init(h2_gizclaw_e2e_ledger_t *ledger, char *buffer,
                              size_t capacity);
void h2_gizclaw_e2e_ledger_append(h2_gizclaw_e2e_ledger_t *ledger,
                                 const char *record, size_t size);
int h2_gizclaw_e2e_ledger_freeze(h2_gizclaw_e2e_ledger_t *ledger);

#endif
