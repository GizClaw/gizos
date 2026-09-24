#ifndef H2_ES8311_POWER_H
#define H2_ES8311_POWER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Synchronous register writer. Return zero on success, nonzero on failure. */
typedef int (*h2_es8311_write_reg_fn)(void *user, uint8_t reg, uint8_t value);

/**
 * @brief Execute the ordered ES8311 suspend sequence without owning a bus.
 * @param user Borrowed callback context; may be NULL if the writer permits it.
 * @param write_reg Non-NULL synchronous writer, called 15 times in order.
 * @return Zero on success, -1 for a NULL writer, otherwise the first callback error.
 *
 * Attempts every write even after an error. The caller stops audio workers and
 * disables PA before calling, retains bus access, and serializes codec access.
 * No allocation, delays, SDK dependency, or physical supply switching. Timeout
 * policy belongs to the writer. On failure the caller may retry the full sequence.
 */
int h2_es8311_suspend(void *user, h2_es8311_write_reg_fn write_reg);

#ifdef __cplusplus
}
#endif
#endif
