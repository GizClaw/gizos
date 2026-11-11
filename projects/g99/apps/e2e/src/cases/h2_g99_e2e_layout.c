#include "h2_g99_e2e_internal.h"

static bool rect_inside(const h2_g99_rect_t* r, int32_t width, int32_t height) {
  return r->x1 >= 0 && r->y1 >= 0 && r->x2 <= width && r->y2 <= height &&
         r->x1 < r->x2 && r->y1 < r->y2;
}

static bool rects_overlap(const h2_g99_rect_t* a, const h2_g99_rect_t* b) {
  return a->x1 < b->x2 && b->x1 < a->x2 && a->y1 < b->y2 && b->y1 < a->y2;
}

/* The wheel and the info area of a settled page stay on screen and apart. */
static h2_pal_result_t check_page(h2_g99_e2e_t* e2e,
                                  const h2_g99_e2e_expect_t* page,
                                  const char* name) {
  h2_g99_observation_t o;
  h2_pal_result_t rc =
      h2_g99_e2e_wait(e2e, page, NULL, NULL, H2_G99_E2E_NAV_MS, name, &o);
  if (rc != H2_PAL_OK) return rc;
  const int32_t w = e2e->target->display_width;
  const int32_t h = e2e->target->display_height;
  if (o.width != w || o.height != h) {
    h2_g99_e2e_fail(e2e, "%s screen %dx%d, target %dx%d", name, (int)o.width,
                    (int)o.height, (int)w, (int)h);
    return H2_PAL_ERR_INVALID_STATE;
  }
  const h2_g99_rect_t* wheel = &o.wheel_rect;
  const h2_g99_rect_t* info = &o.info_rect;
  if (!rect_inside(wheel, w, h) || !rect_inside(info, w, h) ||
      rects_overlap(wheel, info)) {
    h2_g99_e2e_fail(e2e, "%s wheel=%d,%d,%d,%d info=%d,%d,%d,%d screen=%dx%d",
                    name, (int)wheel->x1, (int)wheel->y1, (int)wheel->x2,
                    (int)wheel->y2, (int)info->x1, (int)info->y1, (int)info->x2,
                    (int)info->y2, (int)w, (int)h);
    return H2_PAL_ERR_INVALID_STATE;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t page_geometry(h2_g99_e2e_t* e2e) {
  const h2_g99_e2e_expect_t menu = h2_g99_e2e_main_ready(H2_G99_MAIN_LIBRARY);
  h2_pal_result_t rc = check_page(e2e, &menu, "MAIN");
  const h2_g99_e2e_expect_t library =
      h2_g99_e2e_page_ready(H2_G99_ROUTE_LIBRARY, 0);
  if (rc == H2_PAL_OK) rc = h2_g99_e2e_press(e2e, H2_G99_COMPONENT_OK);
  if (rc == H2_PAL_OK) rc = check_page(e2e, &library, "LIBRARY");
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_HOME, &menu,
                                H2_G99_E2E_NAV_MS, "MAIN(Library)");
  if (rc == H2_PAL_OK) rc = h2_g99_e2e_open_setting(e2e, 0u);
  const h2_g99_e2e_expect_t settings =
      h2_g99_e2e_page_ready(H2_G99_ROUTE_SETTINGS, 0);
  if (rc == H2_PAL_OK) rc = check_page(e2e, &settings, "SETTINGS");
  const h2_g99_e2e_expect_t back = h2_g99_e2e_main_ready(H2_G99_MAIN_SETTINGS);
  if (rc == H2_PAL_OK)
    rc = h2_g99_e2e_press_until(e2e, H2_G99_COMPONENT_HOME, &back,
                                H2_G99_E2E_NAV_MS, "MAIN(Settings)");
  if (rc == H2_PAL_OK) rc = h2_g99_e2e_main_select(e2e, H2_G99_MAIN_LIBRARY);
  return rc;
}

static const h2_g99_e2e_case_t kCases[] = {
    {"page-geometry", 0u, page_geometry},
};

const h2_g99_e2e_group_t h2_g99_e2e_layout_group = {
    "layout", kCases, sizeof(kCases) / sizeof(kCases[0])};
