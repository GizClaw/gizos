#include "h2_g99_e2e_internal.h"

/* The boot animation lasts 900 ms. Observation times are taken when Main
 * publishes, a few ms after its own clock, so allow that much slack. */
#define BOOT_MS 900u
#define BOOT_SLACK_MS 30u
#define MENU_READY_MS 5000u

static bool booting(const h2_g99_observation_t* o, const void* arg) {
  (void)arg;
  return o->route == H2_G99_ROUTE_BOOT &&
         o->transition == H2_G99_TRANSITION_BOOT;
}

/* Main reaches MAIN(Library) only after the full animation, and keys pressed
 * during it are dropped rather than replayed afterwards. */
static h2_pal_result_t boot_animation(h2_g99_e2e_t* e2e) {
  h2_pal_result_t rc = h2_g99_e2e_main_restart(e2e);
  if (rc != H2_PAL_OK) return rc;
  h2_g99_observation_t o;
  rc = h2_g99_e2e_wait(e2e, NULL, booting, NULL, H2_G99_E2E_START_MS,
                       "boot animation start", &o);
  if (rc != H2_PAL_OK) return rc;
  uint64_t boot_seen_ms = 0u;
  (void)h2_g99_e2e_observe(e2e, NULL, &boot_seen_ms);
  if (o.input_ready) {
    h2_g99_e2e_fail(e2e, "input ready during boot animation");
    return H2_PAL_ERR_INVALID_STATE;
  }
  /* DOWN would rotate away from Library and OK would enter it. */
  rc = h2_g99_e2e_press(e2e, H2_G99_COMPONENT_DOWN);
  if (rc == H2_PAL_OK) rc = h2_g99_e2e_press(e2e, H2_G99_COMPONENT_OK);
  if (rc != H2_PAL_OK) return rc;
  if (!h2_g99_e2e_observe(e2e, &o, NULL) || !booting(&o, NULL)) {
    e2e->error = true;
    h2_g99_e2e_fail(e2e, "boot ended before the injected keys were released");
    return H2_PAL_ERR_TIMEOUT;
  }
  const h2_g99_e2e_expect_t menu = h2_g99_e2e_main_ready(H2_G99_MAIN_LIBRARY);
  rc = h2_g99_e2e_wait(e2e, &menu, NULL, NULL, BOOT_MS + 2000u,
                       "MAIN(Library) after boot", NULL);
  if (rc != H2_PAL_OK) return rc;
  uint64_t ready_ms = 0u;
  (void)h2_g99_e2e_observe(e2e, NULL, &ready_ms);
  if (ready_ms - boot_seen_ms + BOOT_SLACK_MS < BOOT_MS) {
    h2_g99_e2e_fail(e2e, "menu ready %llu ms after boot start",
                    (unsigned long long)(ready_ms - boot_seen_ms));
    return H2_PAL_ERR_INVALID_STATE;
  }
  /* Neither key may act now that input is ready. */
  return h2_g99_e2e_stays(e2e, &menu, 700u, "MAIN(Library) after boot keys");
}

/* A fresh Main is running on its menu with input ready and its settings
 * store readable within a bounded time; no network is involved. */
static h2_pal_result_t menu_ready(h2_g99_e2e_t* e2e) {
  h2_pal_result_t rc = h2_g99_e2e_main_restart(e2e);
  if (rc != H2_PAL_OK) return rc;
  h2_g99_observation_t o;
  const h2_g99_e2e_expect_t menu = h2_g99_e2e_main_ready(H2_G99_MAIN_LIBRARY);
  rc = h2_g99_e2e_wait(e2e, &menu, NULL, NULL, MENU_READY_MS, "menu ready", &o);
  if (rc != H2_PAL_OK) return rc;
  if (!o.settings_loaded) {
    h2_g99_e2e_fail(e2e, "settings store not readable");
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (o.width != e2e->target->display_width ||
      o.height != e2e->target->display_height) {
    h2_g99_e2e_fail(e2e, "screen %dx%d, target %dx%d", (int)o.width,
                    (int)o.height, (int)e2e->target->display_width,
                    (int)e2e->target->display_height);
    return H2_PAL_ERR_INVALID_STATE;
  }
  return H2_PAL_OK;
}

static const h2_g99_e2e_case_t kCases[] = {
    {"boot-animation", 0u, boot_animation},
    {"menu-ready", 0u, menu_ready},
};

const h2_g99_e2e_group_t h2_g99_e2e_startup_group = {
    "startup", kCases, sizeof(kCases) / sizeof(kCases[0])};
