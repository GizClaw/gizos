#ifndef H2_GIZCLAW_E2E_SOCIAL_PING_H
#define H2_GIZCLAW_E2E_SOCIAL_PING_H
#include "h2_gizclaw_e2e_internal.h"
/* Runs while the owning Friend/Group case still holds its temporary relation. */
int h2_gizclaw_e2e_check_social_ping(h2_gizclaw_e2e_fixture_t *fixture,
                                    bool group, bool request_api);
#endif
