#ifndef H2_MODEM_RX_H
#define H2_MODEM_RX_H

#include "h2_modem_urc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef h2_pal_result_t (*h2_modem_rx_line_fn)(void *user, const char *line);

typedef struct h2_modem_rx {
    uint64_t next_offset;
    size_t length;
    uint8_t discarding;
    char line[H2_MODEM_URC_LINE_MAX];
} h2_modem_rx_t;

/** @brief Frame an ordered physical RX stream, ignoring replayed byte offsets.
 * Zero-initialize caller-owned state before enabling RX. Calls, reset and
 * destruction must be externally serialized; callback must not reenter.
 * offset identifies data[0] in the physical stream, not a callback sequence.
 * Feed incremental chunks at next_offset; cumulative snapshots retain their
 * original base offset. Transport must supply authoritative offsets: identical
 * bytes or pointers cannot distinguish replay from a new physical occurrence.
 * CR or LF terminates a line; empty lines are ignored and tails are retained.
 * NUL/oversized lines are discarded through the next delimiter. Gaps return
 * INVALID_STATE without consuming data; restart with zeroed state only at a
 * known stream boundary after stopping the producer.
 * Callback borrows a complete NUL-terminated line only during this call and
 * must not block. All input is consumed even if a callback fails; first error
 * is returned, never retry the snapshot to redeliver a failed notification.
 * No allocation, logging, content deduplication or solicited filtering occurs.
 */
h2_pal_result_t h2_modem_rx_feed(
    h2_modem_rx_t *receiver,
    uint64_t offset,
    const uint8_t *data,
    size_t length,
    h2_modem_rx_line_fn handler,
    void *user);

#ifdef __cplusplus
}
#endif
#endif
