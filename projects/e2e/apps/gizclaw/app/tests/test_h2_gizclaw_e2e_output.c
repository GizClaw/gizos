#include "h2_gizclaw_e2e_output.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

static unsigned observed, failures;
static char captured[256];
static void observe(void *user, const char *record, size_t size) {
  assert(user == captured);
  if (record == NULL) { ++failures; return; }
  assert(size < sizeof(captured));
  memcpy(captured, record, size);captured[size] = '\0';++observed;
}
int main(void) {
  h2_gizclaw_e2e_set_evidence_observer(observe, captured);
  assert(h2_gizclaw_e2e_emit("H2_GIZCLAW_E2E symbol=%s rc=%d\n", "test", -6) > 0);
  assert(observed == 1u && strcmp(captured, "H2_GIZCLAW_E2E symbol=test rc=-6\n") == 0);
  char large[2048];memset(large, 'x', sizeof(large) - 1u);large[sizeof(large) - 1u] = '\0';
  assert(h2_gizclaw_e2e_emit("%s", large) < 0);
  assert(failures == 1u && observed == 1u);
  h2_gizclaw_e2e_set_evidence_observer(NULL, NULL);
  assert(h2_gizclaw_e2e_emit("H2_GIZCLAW_E2E stage=default-output\n") > 0);
  assert(observed == 1u && failures == 1u);
  return 0;
}
