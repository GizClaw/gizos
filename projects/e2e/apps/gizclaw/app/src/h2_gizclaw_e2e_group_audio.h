#ifndef H2_GIZCLAW_E2E_GROUP_AUDIO_H
#define H2_GIZCLAW_E2E_GROUP_AUDIO_H

#include "h2_gizclaw_e2e_internal.h"

/* Exercise both download APIs on an existing audio-bearing group message.
 * The fixture owns sink state until all Services have stopped. Single use. */
int h2_gizclaw_e2e_run_group_audio(h2_gizclaw_e2e_fixture_t *fixture,
                                   h2_gizclaw_resp_storage_t *storage,
                                   h2_gizclaw_str_t history_id);

#endif
