#ifndef H2_GIZCLAW_E2E_RPC_H
#define H2_GIZCLAW_E2E_RPC_H

#include "h2_gizclaw_e2e_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

int h2_gizclaw_e2e_run_rpc(h2_gizclaw_e2e_fixture_t *fixture);
int h2_gizclaw_e2e_run_connectivity(h2_gizclaw_e2e_fixture_t *fixture);
int h2_gizclaw_e2e_prepare_voice(h2_gizclaw_e2e_fixture_t *fixture);
int h2_gizclaw_e2e_select_workflow_name(
    const h2_gizclaw_workflow_page_t *workflows, char *out_name,
    size_t out_name_capacity);
bool h2_gizclaw_e2e_workspace_response_ready(
    const h2_gizclaw_workspace_t *workspace, const char *expected_name);

#ifdef __cplusplus
}
#endif

#endif
