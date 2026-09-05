#ifndef H2_GIZCLAW_E2E_VOICE_H
#define H2_GIZCLAW_E2E_VOICE_H

#include "h2_gizclaw_e2e_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

int h2_gizclaw_e2e_run_voice(h2_gizclaw_e2e_fixture_t *fixture);

/* Upload PCM to this run's group Workspace; publish a newly persisted audio
 * history ID only after hooks and Track borrows have been safely released. */
int h2_gizclaw_e2e_generate_group_message(h2_gizclaw_e2e_fixture_t *fixture,
                                          char *history_id, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
