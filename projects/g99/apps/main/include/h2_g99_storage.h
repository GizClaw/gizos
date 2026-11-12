#ifndef H2_G99_STORAGE_H
#define H2_G99_STORAGE_H

#include "core/h2_pal_errors.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_g99_storage_kind {
  H2_G99_STORAGE_UNKNOWN = 0,
  H2_G99_STORAGE_INTERNAL_FLASH,
  H2_G99_STORAGE_NAND,
  H2_G99_STORAGE_SPI_FLASH,
} h2_g99_storage_kind_t;

typedef struct h2_g99_storage_volume {
  h2_g99_storage_kind_t kind;
  h2_pal_result_t result;
  uint64_t used_bytes;
  uint64_t total_bytes;
} h2_g99_storage_volume_t;

typedef struct h2_g99_storage_api {
  void* user;
  h2_g99_storage_kind_t external_kind;
  bool simulated;
  /** Worker-only synchronous query. Each volume reports its own availability;
   * successful usage satisfies used_bytes <= total_bytes and total_bytes > 0.
   * No formatting, erase, garbage collection or data writes are permitted.
   * The callback and user are borrowed until Main's worker has joined. */
  h2_pal_result_t (*query)(void* user, h2_g99_storage_volume_t* internal,
                           h2_g99_storage_volume_t* external);
} h2_g99_storage_api_t;

#ifdef __cplusplus
}
#endif

#endif
