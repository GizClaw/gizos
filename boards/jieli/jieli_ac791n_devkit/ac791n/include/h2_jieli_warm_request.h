#ifndef H2_JIELI_WARM_REQUEST_H
#define H2_JIELI_WARM_REQUEST_H

#include "h2_jieli_wl82_boot.h"

/* Physical double-bank geometry supplied by this board's packer. */
#define H2_JIELI_BANK_1_SFC_BASE UINT32_C(0x00004020)
#define H2_JIELI_BANK_2_SFC_BASE UINT32_C(0x0037c020)

/* Compatibility helpers for board diagnostic fixtures. Native consumers use
 * the board geometry accessor and the platform implementation directly. */
static inline int h2_jieli_warm_request_is_active_image(
    const volatile h2_jieli_warm_request_t* request, uint32_t mapped_base) {
  return h2_jieli_warm_request_active_at(request, mapped_base,
                                         H2_JIELI_BANK_2_SFC_BASE);
}
static inline uint32_t h2_jieli_warm_request_running_base(
    const volatile h2_jieli_warm_request_t* request, uint32_t mapped_base,
    int native_result, uint32_t native_base) {
  return h2_jieli_warm_request_mapped_base(
      request, mapped_base, native_result, native_base,
      H2_JIELI_BANK_1_SFC_BASE, H2_JIELI_BANK_2_SFC_BASE);
}
static inline uint32_t h2_jieli_warm_request_take(
    volatile h2_jieli_warm_request_t* request) {
  return h2_jieli_warm_request_consume(request, H2_JIELI_BANK_1_SFC_BASE,
                                       H2_JIELI_BANK_2_SFC_BASE);
}
static inline void h2_jieli_warm_boot_request(uint32_t base) {
  h2_jieli_wl82_warm_boot_request(base);
}

#endif
