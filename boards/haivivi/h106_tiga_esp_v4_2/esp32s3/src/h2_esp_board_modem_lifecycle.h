#ifndef H2_ESP_BOARD_MODEM_LIFECYCLE_H
#define H2_ESP_BOARD_MODEM_LIFECYCLE_H

#include "h2_pal.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
  h2_pal_result_t (*open)(void* user, uint32_t timeout_ms);
  h2_pal_result_t (*close)(void* user, uint32_t timeout_ms);
  void (*failed_open)(void* user, h2_pal_result_t error);
} h2_board_modem_lifecycle_ops_t;

/* Resolve the default before open records a deadline or waits for its lock.
 * BUSY is admission failure, never permission to cut an OTA-held power rail. */
static inline h2_pal_result_t h2_board_modem_lifecycle_run(
    const h2_board_modem_lifecycle_ops_t* ops, void* user, uint32_t timeout_ms,
    bool opening) {
  if (opening && timeout_ms == 0u) timeout_ms = 45000u;
  const h2_pal_result_t rc =
      opening ? ops->open(user, timeout_ms) : ops->close(user, timeout_ms);
  if (opening && rc != H2_PAL_OK && rc != H2_PAL_ERR_BUSY)
    ops->failed_open(user, rc);
  return rc;
}

typedef struct {
  h2_pal_result_t (*lock)(uint32_t timeout_ms);
  h2_pal_result_t (*admit)(void);
  h2_pal_result_t (*close)(void* user, uint32_t timeout_ms);
  h2_pal_result_t (*unlock)(void);
} h2_board_modem_close_ops_t;

/* The admitted close owns all effects, including power-request changes.
 * Admission and those effects share the provider operation lock. */
static inline h2_pal_result_t h2_board_modem_close_run(
    const h2_board_modem_close_ops_t* ops, void* user, uint32_t timeout_ms) {
  h2_pal_result_t rc = ops->lock(timeout_ms);
  if (rc != H2_PAL_OK) return rc;
  rc = ops->admit();
  if (rc == H2_PAL_OK) rc = ops->close(user, timeout_ms);
  const h2_pal_result_t unlock_rc = ops->unlock();
  return rc != H2_PAL_OK ? rc : unlock_rc;
}

#endif
