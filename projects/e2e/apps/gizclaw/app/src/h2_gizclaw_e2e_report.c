#include "h2_gizclaw_e2e_report.h"

#include "h2/pal/core/h2_pal_errors.h"

#include <string.h>

static h2_gizclaw_e2e_case_result_t *find_case(h2_gizclaw_e2e_report_t *report,
                                               const char *case_id) {
  if (report == NULL || case_id == NULL) {
    return NULL;
  }
  for (size_t index = 0u; index < report->selected; ++index) {
    if (strcmp(report->cases[index].id, case_id) == 0) {
      return &report->cases[index];
    }
  }
  return NULL;
}

void h2_gizclaw_e2e_report_init(h2_gizclaw_e2e_report_t *report) {
  if (report != NULL) {
    memset(report, 0, sizeof(*report));
  }
}

int h2_gizclaw_e2e_report_select(h2_gizclaw_e2e_report_t *report,
                                 const char *case_id) {
  if (report == NULL || case_id == NULL || case_id[0] == '\0') {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (find_case(report, case_id) != NULL) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (report->selected == H2_GIZCLAW_E2E_REPORT_CASES_MAX) {
    return H2_PAL_ERR_NO_SPACE;
  }
  report->cases[report->selected++] = (h2_gizclaw_e2e_case_result_t){
      .id = case_id,
      .status = H2_GIZCLAW_E2E_CASE_PENDING,
  };
  return H2_PAL_OK;
}

int h2_gizclaw_e2e_report_terminal(h2_gizclaw_e2e_report_t *report,
                                   const char *case_id,
                                   h2_gizclaw_e2e_case_status_t status, int rc,
                                   const char *blocked_by) {
  h2_gizclaw_e2e_case_result_t *result = find_case(report, case_id);
  if (result == NULL || status < H2_GIZCLAW_E2E_CASE_PASS ||
      status > H2_GIZCLAW_E2E_CASE_CANCELLED ||
      (status == H2_GIZCLAW_E2E_CASE_BLOCKED &&
       (blocked_by == NULL || blocked_by[0] == '\0'))) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  result->terminal_records++;
  if (result->terminal_records != 1u ||
      result->status != H2_GIZCLAW_E2E_CASE_PENDING) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  result->status = status;
  result->rc = rc;
  result->blocked_by = blocked_by;
  return H2_PAL_OK;
}

void h2_gizclaw_e2e_report_cleanup(h2_gizclaw_e2e_report_t *report,
                                   int cleanup_rc) {
  if (report == NULL) {
    return;
  }
  report->cleanup_recorded = true;
  report->cleanup_rc = cleanup_rc;
}

h2_gizclaw_e2e_summary_t
h2_gizclaw_e2e_report_summarize(const h2_gizclaw_e2e_report_t *report) {
  h2_gizclaw_e2e_summary_t summary = {0};
  if (report == NULL) {
    summary.first_failure_rc = H2_PAL_ERR_INVALID_ARG;
    return summary;
  }
  summary.selected = report->selected;
  summary.cleanup_rc = report->cleanup_rc;
  summary.complete = report->cleanup_recorded;
  for (size_t index = 0u; index < report->selected; ++index) {
    const h2_gizclaw_e2e_case_result_t *result = &report->cases[index];
    if (result->terminal_records != 1u ||
        result->status == H2_GIZCLAW_E2E_CASE_PENDING) {
      summary.complete = false;
      continue;
    }
    summary.terminal++;
    switch (result->status) {
    case H2_GIZCLAW_E2E_CASE_PASS:
      summary.pass++;
      break;
    case H2_GIZCLAW_E2E_CASE_FAIL:
      summary.fail++;
      break;
    case H2_GIZCLAW_E2E_CASE_ERROR:
      summary.error++;
      break;
    case H2_GIZCLAW_E2E_CASE_BLOCKED:
      summary.blocked++;
      break;
    case H2_GIZCLAW_E2E_CASE_CANCELLED:
      summary.cancelled++;
      break;
    case H2_GIZCLAW_E2E_CASE_PENDING:
      break;
    }
    if (summary.first_failure_case == NULL &&
        result->status != H2_GIZCLAW_E2E_CASE_PASS) {
      summary.first_failure_case = result->id;
      summary.first_failure_rc = result->rc;
    }
  }
  summary.complete = summary.complete && summary.terminal == summary.selected;
  return summary;
}

int h2_gizclaw_e2e_summary_exit_code(const h2_gizclaw_e2e_summary_t *summary) {
  if (summary == NULL || !summary->complete || summary->error != 0u ||
      summary->cancelled != 0u || summary->cleanup_rc != H2_PAL_OK) {
    return 2;
  }
  return summary->fail == 0u && summary->blocked == 0u ? 0 : 1;
}

const char *
h2_gizclaw_e2e_case_status_name(h2_gizclaw_e2e_case_status_t status) {
  switch (status) {
  case H2_GIZCLAW_E2E_CASE_PENDING:
    return "PENDING";
  case H2_GIZCLAW_E2E_CASE_PASS:
    return "PASS";
  case H2_GIZCLAW_E2E_CASE_FAIL:
    return "FAIL";
  case H2_GIZCLAW_E2E_CASE_ERROR:
    return "ERROR";
  case H2_GIZCLAW_E2E_CASE_BLOCKED:
    return "BLOCKED";
  case H2_GIZCLAW_E2E_CASE_CANCELLED:
    return "CANCELLED";
  }
  return "UNKNOWN";
}
