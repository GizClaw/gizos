#ifndef H2_GIZCLAW_E2E_CONCURRENCY_H
#define H2_GIZCLAW_E2E_CONCURRENCY_H

#include "h2_gizclaw_e2e_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

int h2_gizclaw_e2e_run_concurrency(h2_gizclaw_e2e_fixture_t *fixture);

int h2_gizclaw_e2e_concurrency_classify(
    int requests_result, int recovery_result, int observation_result,
    size_t started_requests, size_t completed_requests,
    size_t max_open_channels, size_t unique_stream_ids, size_t open_channels);

#ifdef __cplusplus
}
#endif

#endif
