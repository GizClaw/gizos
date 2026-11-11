#include "h2_g99_e2e_internal.h"

#include <stdarg.h>
#include <stdio.h>

/* One record per line on stdout:
 *   H2_E2E_START    selection, capabilities and target
 *   H2_E2E_CASE     one terminal status per selected case
 *   H2_E2E_METRIC   measurements of the current case (touch and perf)
 *   H2_E2E_SUMMARY  the canonical run result and exit code */

void h2_g99_e2e_record(h2_g99_e2e_t* e2e, const char* format, ...) {
  va_list args;
  va_start(args, format);
  const int written = vprintf(format, args);
  va_end(args);
  if ((written < 0 || fflush(stdout) != 0) &&
      e2e->summary.output_rc == H2_PAL_OK)
    e2e->summary.output_rc = H2_PAL_ERR_IO;
}

void h2_g99_e2e_metric(h2_g99_e2e_t* e2e, const char* format, ...) {
  char fields[256];
  va_list args;
  va_start(args, format);
  (void)vsnprintf(fields, sizeof(fields), format, args);
  va_end(args);
  h2_g99_e2e_record(e2e,
                    "H2_E2E_METRIC schema=%u product=g99 suite=g99 case=%s %s "
                    "board=%s target=%s\n",
                    H2_G99_E2E_SCHEMA, e2e->case_id, fields, e2e->target->board,
                    e2e->target->target);
}

static const char* status_name(h2_g99_e2e_status_t status) {
  switch (status) {
    case H2_G99_E2E_PASS:
      return "PASS";
    case H2_G99_E2E_FAIL:
      return "FAIL";
    case H2_G99_E2E_ERROR:
      return "ERROR";
    case H2_G99_E2E_UNSUPPORTED:
      return "UNSUPPORTED";
  }
  return "UNKNOWN";
}

/* Keeps a detail on one quoted field. */
static void sanitize(const char* in, char* out, size_t size) {
  size_t i = 0u;
  for (; in != NULL && in[i] != '\0' && i + 1u < size; ++i) {
    const unsigned char c = (unsigned char)in[i];
    out[i] = c < 0x20u || c == '"' || c == '\\' ? '_' : (char)c;
  }
  out[i] = '\0';
}

void h2_g99_e2e_report_start(h2_g99_e2e_t* e2e) {
  const h2_g99_e2e_target_t* t = e2e->target;
  const bool wifi = (t->capabilities & H2_G99_E2E_CAP_WIFI_TEST_NETWORK) != 0u;
  const bool budget = (t->capabilities & H2_G99_E2E_CAP_FRAME_BUDGET) != 0u;
  h2_g99_e2e_record(
      e2e,
      "H2_E2E_START schema=%u product=g99 suite=g99 selected=%zu "
      "select=\"%s\" capabilities=%s%s%s board=%s target=%s size=%dx%d\n",
      H2_G99_E2E_SCHEMA, e2e->summary.selected,
      t->select != NULL ? t->select : "", wifi ? "wifi" : "",
      wifi && budget ? "," : "", budget ? "frame-budget" : (wifi ? "" : "-"),
      t->board, t->target, (int)t->display_width, (int)t->display_height);
}

void h2_g99_e2e_report_case(h2_g99_e2e_t* e2e, h2_g99_e2e_status_t status,
                            h2_pal_result_t rc, uint64_t duration_ms) {
  char detail[H2_G99_E2E_DETAIL_MAX + 1u];
  sanitize(e2e->detail[0] != '\0'             ? e2e->detail
           : status == H2_G99_E2E_PASS        ? "passed"
           : status == H2_G99_E2E_UNSUPPORTED ? "missing capability"
                                              : "no diagnostic detail",
           detail, sizeof(detail));
  h2_g99_e2e_record(
      e2e,
      "H2_E2E_CASE schema=%u product=g99 suite=g99 case=%s status=%s rc=%d "
      "duration_ms=%llu detail=\"%s\" board=%s target=%s\n",
      H2_G99_E2E_SCHEMA, e2e->case_id, status_name(status), (int)rc,
      (unsigned long long)duration_ms, detail, e2e->target->board,
      e2e->target->target);
}

void h2_g99_e2e_report_summary(h2_g99_e2e_t* e2e) {
  const h2_g99_e2e_summary_t* s = &e2e->summary;
  const size_t terminal = s->passed + s->failed + s->errors + s->unsupported;
  h2_g99_e2e_record(
      e2e,
      "H2_E2E_SUMMARY schema=%u product=g99 suite=g99 selected=%zu "
      "terminal=%zu pass=%zu fail=%zu error=%zu unsupported=%zu setup_rc=%d "
      "cleanup_rc=%d fixture_restore_rc=%d output_rc=%d complete=%s "
      "first_failure_case=%s first_failure_rc=%d exit_code=%d "
      "duration_ms=%llu board=%s target=%s\n",
      H2_G99_E2E_SCHEMA, s->selected, terminal, s->passed, s->failed, s->errors,
      s->unsupported, (int)s->setup_rc, (int)s->cleanup_rc,
      (int)s->fixture_restore_rc, (int)s->output_rc,
      s->complete ? "true" : "false",
      e2e->first_failure[0] != '\0' ? e2e->first_failure : "-",
      (int)e2e->first_failure_rc, h2_g99_e2e_exit_code(s),
      (unsigned long long)(h2_g99_e2e_now_ms(e2e) - e2e->run_started_ms),
      e2e->target->board, e2e->target->target);
}

int h2_g99_e2e_exit_code(const h2_g99_e2e_summary_t* summary) {
  if (summary == NULL || !summary->complete || summary->setup_rc != H2_PAL_OK ||
      summary->cleanup_rc != H2_PAL_OK ||
      summary->fixture_restore_rc != H2_PAL_OK ||
      summary->output_rc != H2_PAL_OK)
    return 2;
  return summary->failed != 0u || summary->errors != 0u ? 1 : 0;
}
