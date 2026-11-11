#include "os/h2_pal_mem.h"
#include "os/h2_pal_pref.h"
#include "h2_g99_e2e_internal.h"

#include <stdio.h>
#include <string.h>

/* Removes every key of the E2E settings namespace. Production settings live
 * in another namespace and are never opened. */
static h2_pal_result_t clear_settings(const h2_pal_pref_api_t* pref) {
  if (pref == NULL) return H2_PAL_OK;
  h2_pal_pref_namespace_t* ns = NULL;
  int rc = h2_pal_pref_open(pref, H2_G99_E2E_SETTINGS_NAMESPACE,
                            H2_PAL_PREF_OPEN_READ_WRITE, &ns);
  if (rc == H2_PAL_ERR_UNSUPPORTED) return H2_PAL_OK;
  if (rc != H2_PAL_OK || ns == NULL)
    return rc == H2_PAL_OK ? H2_PAL_ERR_IO : (h2_pal_result_t)rc;
  rc = ns->clear != NULL ? ns->clear(ns) : H2_PAL_ERR_UNSUPPORTED;
  if (rc == H2_PAL_OK && ns->commit != NULL) rc = ns->commit(ns);
  const int close_rc = ns->close != NULL ? ns->close(ns) : H2_PAL_OK;
  return (h2_pal_result_t)(rc != H2_PAL_OK ? rc : close_rc);
}

static void tally(h2_g99_e2e_t* e2e, h2_g99_e2e_status_t status,
                  h2_pal_result_t rc) {
  h2_g99_e2e_summary_t* s = &e2e->summary;
  switch (status) {
    case H2_G99_E2E_PASS:
      ++s->passed;
      return;
    case H2_G99_E2E_UNSUPPORTED:
      ++s->unsupported;
      return;
    case H2_G99_E2E_FAIL:
      ++s->failed;
      break;
    case H2_G99_E2E_ERROR:
      ++s->errors;
      break;
  }
  if (e2e->first_failure[0] == '\0') {
    (void)snprintf(e2e->first_failure, sizeof(e2e->first_failure), "%s",
                   e2e->case_id);
    e2e->first_failure_rc = rc == H2_PAL_OK ? H2_PAL_ERR_INVALID_STATE : rc;
  }
}

static h2_g99_e2e_status_t run_case(h2_g99_e2e_t* e2e,
                                    const h2_g99_e2e_case_t* test,
                                    h2_pal_result_t* out_rc) {
  *out_rc = H2_PAL_OK;
  if ((test->required_capabilities & ~e2e->target->capabilities) != 0u)
    return H2_G99_E2E_UNSUPPORTED;
  /* Every case starts from rest: MAIN(Library) settled, screen on, with a
   * clean touch source. */
  h2_g99_e2e_touch_model(e2e, H2_G99_E2E_TOUCH_CLEAN);
  h2_pal_result_t rc = h2_g99_e2e_recover(e2e, H2_G99_E2E_REST_MS);
  if (rc != H2_PAL_OK) {
    e2e->error = true;
    *out_rc = rc;
    return H2_G99_E2E_ERROR;
  }
  rc = test->run(e2e);
  *out_rc = rc;
  if (rc != H2_PAL_OK) return e2e->error ? H2_G99_E2E_ERROR : H2_G99_E2E_FAIL;
  /* Judge before sending anything: a case must leave Main at rest. */
  rc = h2_g99_e2e_wait_rest(e2e, H2_G99_E2E_AFTER_CASE_MS, "rest after case");
  *out_rc = rc;
  return rc == H2_PAL_OK ? H2_G99_E2E_PASS : H2_G99_E2E_FAIL;
}

static h2_pal_result_t setup(h2_g99_e2e_t* e2e) {
  char unknown[H2_G99_E2E_SELECT_MAX + 1u];
  if (!h2_g99_e2e_select_valid(e2e->target->select, unknown, sizeof(unknown))) {
    h2_g99_e2e_record(e2e, "H2_E2E_SELECT unknown=\"%s\"\n", unknown);
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_pal_result_t rc = clear_settings(e2e->runtime->pref);
  if (rc != H2_PAL_OK) {
    h2_g99_e2e_record(e2e, "H2_E2E_SETUP stage=settings_fixture rc=%d\n",
                      (int)rc);
    return rc;
  }
  rc = h2_g99_e2e_main_start(e2e);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_wait_rest(e2e, H2_G99_E2E_START_MS, "first Main start");
  if (rc != H2_PAL_OK)
    h2_g99_e2e_record(e2e,
                      "H2_E2E_SETUP stage=main_start rc=%d detail=\"%s\"\n",
                      (int)rc, e2e->detail);
  return rc;
}

h2_pal_result_t h2_g99_e2e_run(const h2_g99_e2e_target_t* target,
                               h2_g99_e2e_summary_t* out_summary) {
  if (target == NULL || target->runtime == NULL || target->board == NULL ||
      target->target == NULL || out_summary == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_g99_e2e_t* e2e = h2_pal_mem_alloc(target->runtime->mem, sizeof(*e2e));
  if (e2e == NULL) return H2_PAL_ERR_NO_MEMORY;
  h2_pal_result_t rc = h2_g99_e2e_session_open(e2e, target);
  e2e->run_started_ms = h2_g99_e2e_now_ms(e2e);
  for (size_t g = 0u; g < h2_g99_e2e_group_count; ++g) {
    const h2_g99_e2e_group_t* group = h2_g99_e2e_groups[g];
    for (size_t c = 0u; c < group->count; ++c)
      if (h2_g99_e2e_selected(target->select, group->name,
                              group->cases[c].name))
        ++e2e->summary.selected;
  }
  h2_g99_e2e_report_start(e2e);
  if (rc != H2_PAL_OK)
    h2_g99_e2e_record(e2e, "H2_E2E_SETUP stage=session rc=%d\n", (int)rc);
  if (rc == H2_PAL_OK) rc = setup(e2e);
  e2e->summary.setup_rc = rc;

  size_t terminal = 0u;
  for (size_t g = 0u; rc == H2_PAL_OK && g < h2_g99_e2e_group_count; ++g) {
    const h2_g99_e2e_group_t* group = h2_g99_e2e_groups[g];
    for (size_t c = 0u; c < group->count; ++c) {
      const h2_g99_e2e_case_t* test = &group->cases[c];
      if (!h2_g99_e2e_selected(target->select, group->name, test->name))
        continue;
      if (h2_g99_e2e_aborted(e2e)) break;
      (void)snprintf(e2e->case_id, sizeof(e2e->case_id), "%s/%s", group->name,
                     test->name);
      e2e->detail[0] = '\0';
      e2e->error = false;
      const uint64_t started = h2_g99_e2e_now_ms(e2e);
      h2_pal_result_t case_rc = H2_PAL_OK;
      const h2_g99_e2e_status_t status = run_case(e2e, test, &case_rc);
      h2_g99_e2e_report_case(e2e, status, case_rc,
                             h2_g99_e2e_now_ms(e2e) - started);
      tally(e2e, status, case_rc);
      ++terminal;
    }
  }
  e2e->case_id[0] = '\0';

  e2e->summary.cleanup_rc = h2_g99_e2e_main_stop(e2e);
  h2_g99_e2e_session_close(e2e);
  e2e->summary.fixture_restore_rc = clear_settings(target->runtime->pref);
  e2e->summary.complete =
      e2e->summary.setup_rc == H2_PAL_OK && terminal == e2e->summary.selected;
  h2_g99_e2e_report_summary(e2e);
  *out_summary = e2e->summary;
  h2_pal_mem_free(target->runtime->mem, e2e);
  return H2_PAL_OK;
}
