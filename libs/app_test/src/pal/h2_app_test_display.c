#include "h2_app_test_display.h"
#include <string.h>
static int open_display(void *u) {
  h2_app_test_display_t *d = u;
  int rc = h2_app_test_fault_take(&d->open);
  if (!rc)
    d->opened = true;
  return rc;
}
static int close_display(void *u) {
  h2_app_test_display_t *d = u;
  int rc = h2_app_test_fault_take(&d->close);
  if (!rc)
    d->opened = false;
  return rc;
}
static int info(void *u, h2_display_info_t *out) {
  h2_app_test_display_t *d = u;
  memset(out, 0, sizeof(*out));
  if (!d->opened)
    return H2_PAL_ERR_INVALID_STATE;
  *out = d->info;
  return 0;
}
static int brightness(void *u, uint32_t percent) {
  h2_app_test_display_t *d = u;
  if (percent > 100u)
    return H2_PAL_ERR_INVALID_ARG;
  if (!d->opened)
    return H2_PAL_ERR_INVALID_STATE;
  d->last_brightness_percent = percent;
  int rc = h2_app_test_fault_take(&d->brightness);
  if (!rc)
    d->brightness_percent = percent;
  return rc;
}
static int draw(void *u, const h2_display_rect_t *r, const void *p, size_t s,
                h2_display_pixel_format_t f) {
  (void)u;
  (void)r;
  (void)p;
  (void)s;
  (void)f;
  return H2_PAL_ERR_UNSUPPORTED;
}
static int present(void *u) {
  (void)u;
  return H2_PAL_ERR_UNSUPPORTED;
}
static const h2_pal_display_vtable_t vtable = {.open = open_display,
                                               .close = close_display,
                                               .get_info = info,
                                               .set_brightness_percent =
                                                   brightness,
                                               .draw_bitmap = draw,
                                               .present = present};
void h2_app_test_display_init(h2_app_test_display_t *d) {
  if (!d)
    return;
  memset(d, 0, sizeof(*d));
  d->api = (h2_pal_display_api_t){d, &vtable};
}
