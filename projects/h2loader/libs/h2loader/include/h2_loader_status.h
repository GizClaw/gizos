#ifndef H2_LOADER_STATUS_H
#define H2_LOADER_STATUS_H

#include "h2_loader_boot.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_LOADER_STATUS_LINE_MAX 4096u

int h2_loader_status_set_active(
    h2_loader_status_t *status,
    const char *role,
    const char *name,
    const char *version,
    const char *checksum);
int h2_loader_status_set_device(
    h2_loader_status_t *status,
    const char *board,
    const char *target,
    const char *chip);
const char *h2_loader_mfg_state_name(const h2_loader_mfg_summary_t *summary);
/** Validates the fixed total and all four-state MFG slots. */
int h2_loader_mfg_summary_validate(const h2_loader_mfg_summary_t *summary);
/** Returns non-zero only when every required MFG slot is PASSED. */
int h2_loader_mfg_summary_is_passed(
    const h2_loader_mfg_summary_t *summary,
    uint32_t required_total);
/**
 * Decides whether completed MFG should hand off to normal Loader work.
 *
 * staged_loader must be 0 or 1. On success, out_pending receives exactly 0 or
 * 1. Both status and out_pending remain caller-owned. Returns
 * H2_PAL_ERR_INVALID_ARG when status or out_pending is null, or when
 * staged_loader is outside 0..1.
 */
int h2_loader_status_mfg_handoff_pending(
    const h2_loader_status_t *status,
    int staged_loader,
    int *out_pending);
/** Copies a validated MFG snapshot into caller-owned Loader status. */
int h2_loader_status_set_mfg(
    h2_loader_status_t *status,
    const h2_loader_mfg_summary_t *summary);
int h2_loader_status_format(
    const h2_loader_status_t *status,
    char *out,
    size_t out_len);

#ifdef __cplusplus
}
#endif

#endif
