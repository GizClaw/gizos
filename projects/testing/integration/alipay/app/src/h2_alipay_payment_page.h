#ifndef H2_ALIPAY_PAYMENT_PAGE_H
#define H2_ALIPAY_PAYMENT_PAGE_H
#include "h2_alipay_e2e.h"
#include "lvgl.h"
enum { H2_ALIPAY_PAYMENT_PAGE_UNBOUND = 1 };
/** Borrow existing App/LVGL-thread display until return. SDK and radio work
 * run on the serialized worker. All tasks/scans are joined/stopped first.
 * Returns UNBOUND after successful local unbind and reminder acknowledgement;
 * the caller must rebuild its binding UI. Otherwise returns OK on exit or a PAL
 * error. */
int h2_alipay_payment_page_run(h2_runtime_t* runtime,
                               const h2_alipay_e2e_config_t* config,
                               lv_display_t* display, int width, int height,
                               int (*presented)(void*), void* presented_user);
#endif
