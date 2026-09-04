#ifndef H2_GIZCLAW_E2E_GROUP_H
#define H2_GIZCLAW_E2E_GROUP_H
#include "h2_gizclaw_e2e_internal.h"

/* Exercise group/invite/member management using both public API forms, then
 * retain a fresh owner group for message and peer-name isolation tests. */
int h2_gizclaw_e2e_run_group_management(h2_gizclaw_e2e_fixture_t *fixture,
                                        h2_gizclaw_resp_storage_t *storage);
#endif
