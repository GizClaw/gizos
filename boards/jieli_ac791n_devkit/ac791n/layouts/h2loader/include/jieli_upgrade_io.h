#ifndef H2_JIELI_UPGRADE_IO_H
#define H2_JIELI_UPGRADE_IO_H

#include <stdint.h>

#define H2_JIELI_UPGRADE_HEADER_SIZE 32u

/* An erase failure poisons this boot, including SDK callbacks that ignore
 * the erase result. Completion and publication must reject it. */
int h2_jieli_upgrade_erase_failed(void);

/* Arm only after candidate payload verification on P1. The gate remains
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
