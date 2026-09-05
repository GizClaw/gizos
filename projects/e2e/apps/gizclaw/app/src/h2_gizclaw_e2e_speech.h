#ifndef H2_GIZCLAW_E2E_SPEECH_H
#define H2_GIZCLAW_E2E_SPEECH_H

#include "h2_gizclaw_e2e_internal.h"

int h2_gizclaw_e2e_run_speech(h2_gizclaw_e2e_fixture_t *fixture,
                              h2_gizclaw_resp_storage_t *storage);
/* Validate the exact schema used by the speech.extract case. */
int h2_gizclaw_e2e_validate_color_json(const h2_pal_mem_api_t *allocator,
                                       h2_gizclaw_str_t json);

#endif
