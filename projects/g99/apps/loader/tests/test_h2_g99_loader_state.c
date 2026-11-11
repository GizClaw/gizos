#include "h2_g99_loader_state.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      (void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                      \
    }                                                                  \
  } while (0)

static void test_phase_percent(void) {
  CHECK(h2_g99_loader_phase_percent(H2_LOADER_INSTALL_PHASE_VALIDATE, 0u,
                                    10u) == 0u);
  CHECK(h2_g99_loader_phase_percent(H2_LOADER_INSTALL_PHASE_VALIDATE, 10u,
                                    10u) == 5u);
  CHECK(h2_g99_loader_phase_percent(H2_LOADER_INSTALL_PHASE_IMAGE, 0u, 100u) ==
        5u);
  CHECK(h2_g99_loader_phase_percent(H2_LOADER_INSTALL_PHASE_IMAGE, 49u, 100u) ==
        41u);
  CHECK(h2_g99_loader_phase_percent(H2_LOADER_INSTALL_PHASE_IMAGE, 100u,
                                    100u) == 80u);
  CHECK(h2_g99_loader_phase_percent(H2_LOADER_INSTALL_PHASE_DATA, 1u, 1u) ==
        85u);
  CHECK(h2_g99_loader_phase_percent(H2_LOADER_INSTALL_PHASE_PIXA, 1u, 2u) ==
        87u);
  CHECK(h2_g99_loader_phase_percent(H2_LOADER_INSTALL_PHASE_VERIFY, 1u, 1u) ==
        99u);
  CHECK(h2_g99_loader_phase_percent(H2_LOADER_INSTALL_PHASE_DONE, 0u, 0u) ==
        100u);
  /* Over-reporting, empty totals and huge values stay in range. */
  CHECK(h2_g99_loader_phase_percent(H2_LOADER_INSTALL_PHASE_IMAGE, 9u, 3u) ==
        80u);
  CHECK(h2_g99_loader_phase_percent(H2_LOADER_INSTALL_PHASE_IMAGE, 0u, 0u) ==
        80u);
  CHECK(h2_g99_loader_phase_percent(H2_LOADER_INSTALL_PHASE_IMAGE,
                                    UINT64_MAX / 2u, UINT64_MAX) == 42u);
  CHECK(h2_g99_loader_phase_percent((h2_loader_install_phase_t)99, 1u, 1u) ==
        0u);
  CHECK(strcmp(h2_g99_loader_phase_name(H2_LOADER_INSTALL_PHASE_IMAGE),
               "Writing image") == 0);
  CHECK(strcmp(h2_g99_loader_phase_name(H2_LOADER_INSTALL_PHASE_VERIFY),
               "Verifying") == 0);
  CHECK(strcmp(h2_g99_loader_phase_name((h2_loader_install_phase_t)0),
               "Installing") == 0);
}

/* Power-on and a normal launch show the brand alone. */
static void test_normal_boot(void) {
  h2_g99_loader_state_t s;
  h2_g99_loader_view_t v;
  h2_g99_loader_state_init(&s);
  h2_g99_loader_state_view(&s, &v);
  CHECK(!v.status_visible && !v.bar_visible && !v.error);
  h2_g99_loader_state_launching(&s);
  h2_g99_loader_state_view(&s, &v);
  CHECK(s.screen == H2_G99_LOADER_SCREEN_BRAND);
  CHECK(!v.status_visible && !v.bar_visible);
}

static void test_ready(void) {
  h2_g99_loader_state_t s;
  h2_g99_loader_view_t v;
  h2_g99_loader_state_init(&s);
  h2_g99_loader_state_ready(&s);
  h2_g99_loader_state_view(&s, &v);
  CHECK(v.status_visible && !v.bar_visible && !v.error);
  CHECK(strcmp(v.status, "Ready to transfer") == 0);
}

static void test_install_then_start(void) {
  h2_g99_loader_state_t s;
  h2_g99_loader_view_t v;
  h2_g99_loader_state_init(&s);
  h2_g99_loader_state_installing(&s);
  h2_g99_loader_state_view(&s, &v);
  CHECK(strcmp(v.status, "Installing GizPocket...") == 0);
  CHECK(!v.bar_visible);
  h2_g99_loader_state_progress(&s, H2_LOADER_INSTALL_PHASE_IMAGE, 49u, 100u);
  h2_g99_loader_state_view(&s, &v);
  CHECK(strcmp(v.status, "Writing image... 41%") == 0);
  CHECK(v.bar_visible && v.bar_percent == 41u && !v.error);
  /* Progress never moves backwards, even when a phase reports less. */
  h2_g99_loader_state_progress(&s, H2_LOADER_INSTALL_PHASE_VALIDATE, 0u, 1u);
  CHECK(s.percent == 41u);
  h2_g99_loader_state_progress(&s, H2_LOADER_INSTALL_PHASE_VERIFY, 3u, 10u);
  h2_g99_loader_state_view(&s, &v);
  CHECK(strcmp(v.status, "Verifying... 92%") == 0);
  h2_g99_loader_state_launching(&s);
  h2_g99_loader_state_view(&s, &v);
  CHECK(s.screen == H2_G99_LOADER_SCREEN_STARTING);
  CHECK(strcmp(v.status, "Starting GizPocket...") == 0);
  CHECK(!v.bar_visible);
}

static void test_failures(void) {
  h2_g99_loader_state_t s;
  h2_g99_loader_view_t v;
  h2_g99_loader_state_init(&s);
  h2_g99_loader_state_installing(&s);
  h2_g99_loader_state_progress(&s, H2_LOADER_INSTALL_PHASE_IMAGE, 49u, 100u);
  h2_g99_loader_state_failed(&s, -5);
  h2_g99_loader_state_view(&s, &v);
  CHECK(strcmp(v.status, "Writing image failed (-5)") == 0);
  CHECK(v.error && v.bar_visible && v.bar_percent == 41u);
  /* A failure before any phase has no progress to keep. */
  h2_g99_loader_state_init(&s);
  h2_g99_loader_state_failed(&s, -3);
  h2_g99_loader_state_view(&s, &v);
  CHECK(strcmp(v.status, "Installing failed (-3)") == 0);
  CHECK(v.error && !v.bar_visible);
  /* A failure after a launch began names the launch. */
  h2_g99_loader_state_init(&s);
  h2_g99_loader_state_installing(&s);
  h2_g99_loader_state_launching(&s);
  h2_g99_loader_state_failed(&s, -7);
  h2_g99_loader_state_view(&s, &v);
  CHECK(strcmp(v.status, "Starting GizPocket failed (-7)") == 0);
  /* A new install clears the old progress. */
  h2_g99_loader_state_installing(&s);
  CHECK(s.percent == 0u && s.phase == 0 && !s.launching);
}

int main(void) {
  test_phase_percent();
  test_normal_boot();
  test_ready();
  test_install_then_start();
  test_failures();
  if (failures != 0) return 1;
  (void)printf("PASS\n");
  return 0;
}
