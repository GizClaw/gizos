#include "h2_gizclaw_e2e_output.h"

#include <stdarg.h>
#include <stdio.h>

/* Installed before case tasks start and cleared only after they quiesce.
 * The existing single-run guard owns this callback and its borrowed context. */
static h2_gizclaw_e2e_evidence_fn s_observer;
static void *s_user;

void h2_gizclaw_e2e_set_evidence_observer(h2_gizclaw_e2e_evidence_fn observer,
                                       void *user) {
  s_observer = observer;
  s_user = user;
}

int h2_gizclaw_e2e_emit(const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  if (s_observer == NULL) {
    int result = vprintf(format, arguments);
    va_end(arguments);
    return result;
  }
  char record[1024];
  int size = vsnprintf(record, sizeof(record), format, arguments);
  va_end(arguments);
  if (size < 0 || (size_t)size >= sizeof(record)) {
    s_observer(s_user, NULL, 0u);
    return -1;
  }
  s_observer(s_user, record, (size_t)size);
  return fputs(record, stdout) < 0 ? -1 : size;
}
