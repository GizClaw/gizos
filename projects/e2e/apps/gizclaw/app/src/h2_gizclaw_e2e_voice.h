#ifndef H2_GIZCLAW_E2E_VOICE_H
#define H2_GIZCLAW_E2E_VOICE_H

#include "h2_gizclaw_e2e_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

int h2_gizclaw_e2e_run_voice(h2_gizclaw_e2e_fixture_t *fixture);

/* Push one PTT utterance into this run's group SFU Workspace. The Workspace
 * forwards the sender's Opus to the other members and returns nothing on the
 * sender's own route, so acceptance is a turn the SFU runtime admits without a
 * typed EOS error, then a clean hangup. */
int h2_gizclaw_e2e_run_group_talk(h2_gizclaw_e2e_fixture_t *fixture);

#ifdef __cplusplus
}
#endif

#endif
