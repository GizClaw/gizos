#include "h2_g99_e2e_internal.h"

/* Perf cases measure Main's frames and touch reads. Targets that declare
 * H2_G99_E2E_CAP_FRAME_BUDGET must hold the budgets; elsewhere the cases
 * only report their numbers in H2_E2E_METRIC lines. */

static bool budgeted(const h2_g99_e2e_t* e2e) {
  return (e2e->target->capabilities & H2_G99_E2E_CAP_FRAME_BUDGET) != 0u;
}

static void report_frames(h2_g99_e2e_t* e2e, const char* phase,
                          uint32_t window_ms, const h2_g99_e2e_frames_t* f) {
  const uint32_t intervals = f->frames > 1u ? f->frames - 1u : 0u;
  h2_g99_e2e_metric(
      e2e,
      "phase=%s window_ms=%u frames=%u interval_avg_us=%u "
      "interval_max_us=%u work_avg_us=%u work_max_us=%u "
      "draw_max_us=%u present_max_us=%u pixels_avg=%u",
      phase, (unsigned)window_ms, (unsigned)f->frames,
      intervals > 0u ? (unsigned)(f->interval_us / intervals) : 0u,
      (unsigned)f->max_interval_us,
      f->frames > 0u ? (unsigned)(f->work_us / f->frames) : 0u,
      (unsigned)f->max_work_us, (unsigned)f->max_draw_us,
      (unsigned)f->max_present_us,
      f->frames > 0u ? (unsigned)(f->pixels / f->frames) : 0u);
}

/* At rest Main has nothing new to draw and keeps reading touch often enough
 * that a quick drag registers. */
static h2_pal_result_t idle_frames(h2_g99_e2e_t* e2e) {
  enum { kWindowMs = 3000 };
  h2_g99_e2e_frames_reset(e2e);
  h2_g99_e2e_touch_stats_t touch = {0u, 0u};
  const h2_pal_result_t rc = h2_g99_e2e_touch_probe(e2e, kWindowMs, &touch);
  h2_g99_e2e_frames_t f;
  h2_g99_e2e_frames_get(e2e, &f);
  report_frames(e2e, "idle", kWindowMs, &f);
  h2_g99_e2e_metric(e2e, "phase=idle touch_reads=%u touch_max_gap_ms=%u",
                    (unsigned)touch.reads, (unsigned)touch.max_gap_ms);
  if (rc != H2_PAL_OK) {
    h2_g99_e2e_fail(e2e, "Main stopped in the idle window");
    return rc;
  }
  if (budgeted(e2e) && touch.max_gap_ms > H2_G99_E2E_TOUCH_GAP_BUDGET_MS) {
    h2_g99_e2e_fail(e2e,
                    "touch read gap %u ms over the %u ms budget (frames=%u "
                    "work_max_us=%u)",
                    (unsigned)touch.max_gap_ms,
                    (unsigned)H2_G99_E2E_TOUCH_GAP_BUDGET_MS,
                    (unsigned)f.frames, (unsigned)f.max_work_us);
    return H2_PAL_ERR_TIMEOUT;
  }
  return H2_PAL_OK;
}

/* A key rotation of the main wheel animates in enough frames. */
static h2_pal_result_t rotate_frames(h2_g99_e2e_t* e2e) {
  h2_g99_e2e_frames_reset(e2e);
  const uint64_t start = h2_g99_e2e_now_ms(e2e);
  const h2_g99_e2e_expect_t lab = h2_g99_e2e_main_ready(H2_G99_MAIN_LAB);
  h2_pal_result_t rc = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_DOWN, &lab,
                                              H2_G99_E2E_NAV_MS, "MAIN(Lab)");
  const uint32_t window_ms = (uint32_t)(h2_g99_e2e_now_ms(e2e) - start);
  h2_g99_e2e_frames_t f;
  h2_g99_e2e_frames_get(e2e, &f);
  report_frames(e2e, "rotate", window_ms, &f);
  const h2_g99_e2e_expect_t library =
      h2_g99_e2e_main_ready(H2_G99_MAIN_LIBRARY);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_UP, &library,
                                H2_G99_E2E_NAV_MS, "MAIN(Library)");
  if (rc == H2_PAL_OK && budgeted(e2e) &&
      f.frames < H2_G99_E2E_ROTATE_MIN_FRAMES) {
    h2_g99_e2e_fail(e2e,
                    "rotation drew %u frames in %u ms, under %u "
                    "(interval_max_us=%u work_max_us=%u)",
                    (unsigned)f.frames, (unsigned)window_ms,
                    (unsigned)H2_G99_E2E_ROTATE_MIN_FRAMES,
                    (unsigned)f.max_interval_us, (unsigned)f.max_work_us);
    return H2_PAL_ERR_TIMEOUT;
  }
  return rc;
}

static const h2_g99_e2e_case_t kCases[] = {
    {"idle-frames", 0u, idle_frames},
    {"rotate-frames", 0u, rotate_frames},
};

const h2_g99_e2e_group_t h2_g99_e2e_perf_group = {
    "perf", kCases, sizeof(kCases) / sizeof(kCases[0])};
