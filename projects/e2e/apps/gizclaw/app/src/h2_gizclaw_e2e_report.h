#ifndef H2_GIZCLAW_E2E_REPORT_H
#define H2_GIZCLAW_E2E_REPORT_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_GIZCLAW_E2E_REPORT_CASES_MAX 16u

typedef enum h2_gizclaw_e2e_case_status {
  H2_GIZCLAW_E2E_CASE_PENDING = 0,
  H2_GIZCLAW_E2E_CASE_PASS,
  H2_GIZCLAW_E2E_CASE_FAIL,
  H2_GIZCLAW_E2E_CASE_ERROR,
  H2_GIZCLAW_E2E_CASE_BLOCKED,
  H2_GIZCLAW_E2E_CASE_CANCELLED,
} h2_gizclaw_e2e_case_status_t;

typedef struct h2_gizclaw_e2e_case_result {
  const char *id;
  const char *blocked_by;
  h2_gizclaw_e2e_case_status_t status;
  int rc;
  unsigned terminal_records;
} h2_gizclaw_e2e_case_result_t;

typedef struct h2_gizclaw_e2e_report {
  h2_gizclaw_e2e_case_result_t cases[H2_GIZCLAW_E2E_REPORT_CASES_MAX];
  size_t selected;
  int cleanup_rc;
  bool cleanup_recorded;
} h2_gizclaw_e2e_report_t;

typedef struct h2_gizclaw_e2e_summary {
  size_t selected;
  size_t pass;
  size_t fail;
  size_t error;
  size_t blocked;
  size_t cancelled;
  size_t terminal;
  const char *first_failure_case;
  int first_failure_rc;
  int cleanup_rc;
  bool complete;
} h2_gizclaw_e2e_summary_t;

void h2_gizclaw_e2e_report_init(h2_gizclaw_e2e_report_t *report);
int h2_gizclaw_e2e_report_select(h2_gizclaw_e2e_report_t *report,
                                 const char *case_id);
int h2_gizclaw_e2e_report_terminal(h2_gizclaw_e2e_report_t *report,
                                   const char *case_id,
                                   h2_gizclaw_e2e_case_status_t status, int rc,
                                   const char *blocked_by);
void h2_gizclaw_e2e_report_cleanup(h2_gizclaw_e2e_report_t *report,
                                   int cleanup_rc);
h2_gizclaw_e2e_summary_t
h2_gizclaw_e2e_report_summarize(const h2_gizclaw_e2e_report_t *report);
int h2_gizclaw_e2e_summary_exit_code(const h2_gizclaw_e2e_summary_t *summary);
const char *
h2_gizclaw_e2e_case_status_name(h2_gizclaw_e2e_case_status_t status);

#ifdef __cplusplus
}
#endif

#endif
