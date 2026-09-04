#include "h2_gizclaw_e2e_report.h"

#include "h2/pal/core/h2_pal_errors.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

static void test_failure_does_not_hide_later_pass(void) {
  h2_gizclaw_e2e_report_t report;
  h2_gizclaw_e2e_report_init(&report);
  assert(h2_gizclaw_e2e_report_select(&report, "first") == H2_PAL_OK);
  assert(h2_gizclaw_e2e_report_select(&report, "later") == H2_PAL_OK);
  assert(h2_gizclaw_e2e_report_terminal(
             &report, "first", H2_GIZCLAW_E2E_CASE_FAIL,
             H2_PAL_ERR_INVALID_STATE, NULL) == H2_PAL_OK);
  assert(h2_gizclaw_e2e_report_terminal(&report, "later",
                                        H2_GIZCLAW_E2E_CASE_PASS, H2_PAL_OK,
                                        NULL) == H2_PAL_OK);
  h2_gizclaw_e2e_report_cleanup(&report, H2_PAL_OK);
  const h2_gizclaw_e2e_summary_t summary =
      h2_gizclaw_e2e_report_summarize(&report);
  assert(summary.complete);
  assert(summary.selected == 2u && summary.terminal == 2u);
  assert(summary.fail == 1u && summary.pass == 1u);
  assert(strcmp(summary.first_failure_case, "first") == 0);
  assert(h2_gizclaw_e2e_summary_exit_code(&summary) == 1);
}

static void test_blocked_cancelled_and_cleanup_mapping(void) {
  h2_gizclaw_e2e_report_t report;
  h2_gizclaw_e2e_report_init(&report);
  assert(h2_gizclaw_e2e_report_select(&report, "dependency") == H2_PAL_OK);
  assert(h2_gizclaw_e2e_report_select(&report, "cancelled") == H2_PAL_OK);
  assert(h2_gizclaw_e2e_report_terminal(
             &report, "dependency", H2_GIZCLAW_E2E_CASE_BLOCKED,
             H2_PAL_ERR_UNAVAILABLE, "setup") == H2_PAL_OK);
  assert(h2_gizclaw_e2e_report_terminal(&report, "cancelled",
                                        H2_GIZCLAW_E2E_CASE_CANCELLED,
                                        H2_PAL_ERR_CLOSED, NULL) == H2_PAL_OK);
  h2_gizclaw_e2e_report_cleanup(&report, H2_PAL_ERR_IO);
  const h2_gizclaw_e2e_summary_t summary =
      h2_gizclaw_e2e_report_summarize(&report);
  assert(summary.complete);
  assert(summary.blocked == 1u && summary.cancelled == 1u);
  assert(h2_gizclaw_e2e_summary_exit_code(&summary) == 2);
}

static void test_duplicate_and_incomplete_are_harness_errors(void) {
  h2_gizclaw_e2e_report_t report;
  h2_gizclaw_e2e_report_init(&report);
  assert(h2_gizclaw_e2e_report_select(&report, "only") == H2_PAL_OK);
  assert(h2_gizclaw_e2e_report_terminal(&report, "only",
                                        H2_GIZCLAW_E2E_CASE_PASS, H2_PAL_OK,
                                        NULL) == H2_PAL_OK);
  assert(h2_gizclaw_e2e_report_terminal(&report, "only",
                                        H2_GIZCLAW_E2E_CASE_PASS, H2_PAL_OK,
                                        NULL) == H2_PAL_ERR_INVALID_STATE);
  h2_gizclaw_e2e_report_cleanup(&report, H2_PAL_OK);
  h2_gizclaw_e2e_summary_t summary = h2_gizclaw_e2e_report_summarize(&report);
  assert(!summary.complete);
  assert(h2_gizclaw_e2e_summary_exit_code(&summary) == 2);

  h2_gizclaw_e2e_report_init(&report);
  assert(h2_gizclaw_e2e_report_select(&report, "pending") == H2_PAL_OK);
  summary = h2_gizclaw_e2e_report_summarize(&report);
  assert(!summary.complete);
  assert(h2_gizclaw_e2e_summary_exit_code(&summary) == 2);
}

int main(void) {
  test_failure_does_not_hide_later_pass();
  test_blocked_cancelled_and_cleanup_mapping();
  test_duplicate_and_incomplete_are_harness_errors();
  return 0;
}
