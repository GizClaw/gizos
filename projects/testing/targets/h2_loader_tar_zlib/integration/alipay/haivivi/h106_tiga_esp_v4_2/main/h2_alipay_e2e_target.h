#ifndef H2_ALIPAY_E2E_TARGET_H
#define H2_ALIPAY_E2E_TARGET_H

#include "h2_runtime.h"

/* Final-entry composition using the public Tiga BSP. Configure
 * the native adapter with actual identity providers; do not initialize it.
 * All borrowed providers must remain alive until hardware reset. */
h2_pal_result_t h2_alipay_e2e_target_configure(h2_runtime_t* runtime);
uint32_t h2_alipay_e2e_target_feature(void);

/* Quiescent-only completed-transfer tap. Pixels are panel-order RGB565.
 * NULL unregisters and queries geometry; no optical verification is implied. */
typedef void (*h2_alipay_e2e_capture_fn)(void* user,
                                         const h2_display_rect_t* rect,
                                         const uint16_t* pixels);
h2_display_info_t h2_alipay_e2e_target_capture(h2_alipay_e2e_capture_fn capture,
                                               void* user);

#endif
