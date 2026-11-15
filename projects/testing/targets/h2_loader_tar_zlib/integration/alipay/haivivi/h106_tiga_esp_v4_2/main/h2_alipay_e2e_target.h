#ifndef H2_ALIPAY_E2E_TARGET_H
#define H2_ALIPAY_E2E_TARGET_H

#include "h2_alipay_payment.h"

#include "h2_runtime.h"
#include "h2_alipay.h"

/* Final-entry composition using the public Tiga BSP. Configure
 * the native adapter with actual identity providers; do not initialize it.
 * All borrowed providers must remain alive until hardware reset. */
h2_pal_result_t h2_alipay_e2e_target_configure(h2_runtime_t* runtime);
uint32_t h2_alipay_e2e_target_feature(void);
bool h2_alipay_e2e_target_complete_binding(void);
const h2_alipay_payment_api_t* h2_alipay_e2e_target_payment_api(void);
const h2_alipay_api_t* h2_alipay_e2e_target_api(void);

/* Non-secret configured identity readback through the original
 * alipay_get_companyName/productModel vendor interfaces. Both backends use
 * the same configured values as registration and signed requests. Zero
 * field results mean success. These diagnostics neither initialize the SDK nor
 * gate binding. Strings are always terminated.
 */
typedef struct h2_alipay_e2e_product_info {
  char company[16];
  char model[24];
  int company_rc;
  int model_rc;
} h2_alipay_e2e_product_info_t;
void h2_alipay_e2e_target_product_info(h2_alipay_e2e_product_info_t* out);

/* Quiescent-only completed-transfer tap. Pixels are panel-order RGB565.
 * NULL unregisters and queries geometry; no optical verification is implied. */
typedef void (*h2_alipay_e2e_capture_fn)(void* user,
                                         const h2_display_rect_t* rect,
                                         const uint16_t* pixels);
h2_display_info_t h2_alipay_e2e_target_capture(h2_alipay_e2e_capture_fn capture,
                                               void* user);

#endif
