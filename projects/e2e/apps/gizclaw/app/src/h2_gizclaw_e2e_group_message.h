#ifndef H2_GIZCLAW_E2E_GROUP_MESSAGE_H
#define H2_GIZCLAW_E2E_GROUP_MESSAGE_H

#include "h2_gizclaw_e2e_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Check the exact user-audio history produced by this run, through both APIs. */
int h2_gizclaw_e2e_run_group_message(h2_gizclaw_e2e_fixture_t *fixture,
                                     h2_gizclaw_resp_storage_t *storage,
                                     h2_gizclaw_str_t history_id);

#ifdef __cplusplus
}
#endif
#endif
