#include "h2_gizclaw_e2e_internal.h"
#include <stdio.h>
#include <string.h>

typedef struct cancel_state {
  h2_gizclaw_e2e_fixture_t *fixture;
  unsigned begins, pages, commits, aborts, cancel_calls;
  bool cancel;
  int cancel_result;
} cancel_state_t;

static int dispose(h2_gizclaw_e2e_fixture_t *f) {
  int rc = f->actors[0].session ? h2_gizclaw_session_close(f->actors[0].session) : H2_PAL_OK;
  if (rc == H2_PAL_OK) rc = h2_gizclaw_session_destroy(&f->actors[0].session);
  if (rc != H2_PAL_OK) return rc;
  h2_pal_mem_free(f->allocator, f->case_state);
  f->case_state = NULL;
  f->case_cleanup = NULL;
  return H2_PAL_OK;
}

static h2_pal_result_t catalog(void *user, h2_gizclaw_catalog_event_t event,
    const h2_gizclaw_workflow_page_t *page, const char *profile, const char *revision) {
  cancel_state_t *state = user;
  if (!profile || strcmp(profile, state->fixture->runtime_profile_name) ||
      !revision || !revision[0]) return H2_PAL_ERR_FORMAT;
  switch (event) {
  case H2_GIZCLAW_CATALOG_BEGIN:
    ++state->begins;
    if (state->cancel) {
      ++state->cancel_calls;
      state->cancel_result = h2_gizclaw_session_cancel_pending(state->fixture->actors[0].session);
      h2_gizclaw_e2e_evidence("h2_gizclaw_session_cancel_pending", "session-cancel", state->cancel_result);
      return state->cancel_result;
    }
    break;
  case H2_GIZCLAW_CATALOG_PAGE:
    if (!page) return H2_PAL_ERR_FORMAT;
    ++state->pages;
    break;
  case H2_GIZCLAW_CATALOG_COMMIT: ++state->commits; break;
  case H2_GIZCLAW_CATALOG_ABORT: ++state->aborts; break;
  default: return H2_PAL_ERR_FORMAT;
  }
  return H2_PAL_OK;
}

int h2_gizclaw_e2e_run_session_cancel(h2_gizclaw_e2e_fixture_t *f) {
  if (!f || !f->allocator || !f->runtime || !f->actors[0].session ||
      !f->actors[0].service || f->case_state || f->case_cleanup) return H2_PAL_ERR_INVALID_ARG;
  if (!h2_gizclaw_e2e_fixture_has_time(f, 90000u)) return H2_PAL_ERR_TIMEOUT;
  int rc = h2_gizclaw_session_close(f->actors[0].session);
  if (rc == H2_PAL_OK) rc = h2_gizclaw_session_destroy(&f->actors[0].session);
  if (rc != H2_PAL_OK) return rc;
  cancel_state_t *state = h2_pal_mem_alloc(f->allocator, sizeof(*state));
  if (!state) return H2_PAL_ERR_NO_MEMORY;
  *state = (cancel_state_t){.fixture=f};
  f->case_state = state;
  f->case_cleanup = dispose;
  static const char *const collections[] = {"assistants"};
  const h2_gizclaw_session_config_t config = {
      .service=f->actors[0].service, .mem=f->allocator, .sync=f->runtime->sync,
      .time=f->time, .collections=collections, .collection_count=1u,
      .catalog_bytes=65536u, .catalog_sink=catalog, .catalog_sink_user=state};
  rc = h2_gizclaw_session_create(&config, &f->actors[0].session);
  if (rc == H2_PAL_OK) rc = h2_gizclaw_session_register(f->actors[0].session, f->registration_token, 30000u);
  h2_gizclaw_session_state_t before = {0}, after = {0};
  if (rc == H2_PAL_OK) rc = h2_gizclaw_session_snapshot(f->actors[0].session, &before);
  if (rc == H2_PAL_OK && (state->begins != 1u || state->commits != 1u ||
      state->aborts || before.catalog != H2_GIZCLAW_SESSION_READY)) rc = H2_PAL_ERR_INVALID_STATE;
  if (rc == H2_PAL_OK) {
    state->cancel = true;
    const int refresh_rc = h2_gizclaw_session_refresh(f->actors[0].session, 30000u);
    rc = h2_gizclaw_session_snapshot(f->actors[0].session, &after);
    if (rc == H2_PAL_OK && (refresh_rc != H2_PAL_ERR_CLOSED ||
        state->cancel_calls != 1u || state->cancel_result != H2_PAL_OK ||
        state->begins != 2u || state->commits != 1u || state->aborts != 1u ||
        after.catalog != H2_GIZCLAW_SESSION_FAILED || after.can_start ||
        after.generation <= before.generation ||
        after.registration != H2_GIZCLAW_SESSION_READY)) rc = H2_PAL_ERR_INVALID_STATE;
  }
  if (rc == H2_PAL_OK) {
    state->cancel = false;
    rc = h2_gizclaw_session_refresh(f->actors[0].session, 30000u);
    if (rc == H2_PAL_OK) rc = h2_gizclaw_session_snapshot(f->actors[0].session, &after);
    if (rc == H2_PAL_OK && (state->begins != 3u || state->commits != 2u ||
        state->aborts != 1u || after.catalog != H2_GIZCLAW_SESSION_READY ||
        after.registration != H2_GIZCLAW_SESSION_READY)) rc = H2_PAL_ERR_INVALID_STATE;
  }
  h2_gizclaw_e2e_emit("H2_GIZCLAW_E2E stage=session-cancel begins=%u commits=%u aborts=%u "
         "result=%s rc=%d\n", state->begins, state->commits, state->aborts,
         rc == H2_PAL_OK ? "PASS" : "FAIL", rc);
  const int cleanup = dispose(f);
  if (rc == H2_PAL_OK) rc = cleanup;
  h2_gizclaw_e2e_evidence("h2_gizclaw_session_cancel_pending", "session_cancel_pending-assert", rc);
  return rc;
}
