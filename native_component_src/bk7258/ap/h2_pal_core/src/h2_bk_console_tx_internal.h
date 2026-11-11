#ifndef H2_BK_CONSOLE_TX_INTERNAL_H
#define H2_BK_CONSOLE_TX_INTERNAL_H

#include "core/h2_pal_errors.h"

#include <stddef.h>
#include <stdint.h>

/* Task-context TX only: shares whole-record arbitration with management without
 * configuring/releasing its RX session. The deadline includes contention and
 * FIFO drain. Accepted prefixes remain observable on failure. */
h2_pal_result_t h2_bk_console_write(const void* buffer, size_t len,
                                    size_t* out_written, uint32_t timeout_ms);

#endif
