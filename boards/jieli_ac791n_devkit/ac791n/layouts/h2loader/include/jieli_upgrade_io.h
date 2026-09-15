#ifndef H2_JIELI_UPGRADE_IO_H
#define H2_JIELI_UPGRADE_IO_H

#include <stdint.h>

#define H2_JIELI_UPGRADE_HEADER_SIZE 32u

/* An erase failure poisons this boot, including SDK callbacks that ignore
 * the erase result. Completion and publication must reject it. */
int h2_jieli_upgrade_erase_failed(void);

/* Torn-header recovery belongs to this board layout and its pinned SDK, not
 * the public Loader: an explicit complete reinstall from P1 erases and verifies
 * the P2 header sector during payload transfer, before arm. Erase failure poisons
 * the boot. Arm/publish never repair or erase a nonempty header (including a
 * valid different bank); replacing such a bank requires a complete install.
 * Arm only after candidate payload verification on P1. The gate remains
 * latched until reset, including after errors: late SDK writes cannot publish
 * a candidate header after the caller has abandoned the transaction. */
int h2_jieli_upgrade_header_arm(void);
int h2_jieli_upgrade_header_copy(uint8_t out[H2_JIELI_UPGRADE_HEADER_SIZE]);

/* Caller must validate persisted image/header identity and confirm the P2
 * Loader before calling. Writes only an erased P2 header, never erases Flash;
 * an identical physical header is an idempotent success. */
int h2_jieli_upgrade_header_publish(
    const uint8_t header[H2_JIELI_UPGRADE_HEADER_SIZE]);

#endif
