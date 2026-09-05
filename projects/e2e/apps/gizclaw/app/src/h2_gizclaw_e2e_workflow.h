#ifndef H2_GIZCLAW_E2E_WORKFLOW_H
#define H2_GIZCLAW_E2E_WORKFLOW_H

#include "h2_gizclaw_e2e_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

int h2_gizclaw_e2e_run_workflow(h2_gizclaw_e2e_fixture_t *fixture,
                                h2_gizclaw_resp_storage_t *storage);
int h2_gizclaw_e2e_select_workflow_name(
    const h2_gizclaw_workflow_page_t *workflows, char *out_name,
    size_t out_name_capacity);

#ifdef __cplusplus
}
#endif

#endif
