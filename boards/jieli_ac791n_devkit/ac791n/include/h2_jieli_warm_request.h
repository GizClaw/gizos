#ifndef H2_JIELI_WARM_REQUEST_H
#define H2_JIELI_WARM_REQUEST_H

#include <stdint.h>

/* Shared h2loader layout ABI: the ROM-selected Loader consumes this one-shot
 * request before starting the common Loader. App reboot must bypass Stage. */
#define H2_JIELI_BANK_1_SFC_BASE UINT32_C(0x00004020)
#define H2_JIELI_BANK_2_SFC_BASE UINT32_C(0x0037c020)
#define H2_JIELI_WARM_REQUEST_ADDR UINT32_C(0x01c7fc00)
#define H2_JIELI_WARM_MAGIC UINT32_C(0x42573248)
#define H2_JIELI_WARM_INFLIGHT UINT32_C(0x464c4e49)

typedef struct h2_jieli_warm_request {
  uint32_t magic;
  uint32_t sfc_base;
  uint32_t check;
  uint32_t count;
  uint32_t last_base;
  uint32_t inflight;
} h2_jieli_warm_request_t;

static inline uint32_t h2_jieli_warm_request_check(uint32_t base) {
  return ~(H2_JIELI_WARM_MAGIC ^ base ^ UINT32_C(0x5a5aa5a5));
}

/* A candidate Loader runs the same early hook as the canonical Loader. Only
 * arrival in the requested P2 mapping is still inside the trial; a ROM reset
 * back to P1 must snapshot the evidence instead. Do not consume it on arrival. */
static inline int h2_jieli_warm_request_is_active_image(
    const volatile h2_jieli_warm_request_t *request, uint32_t mapped_base) {
  return mapped_base == H2_JIELI_BANK_2_SFC_BASE &&
         request->magic == 0u &&
         request->inflight == H2_JIELI_WARM_INFLIGHT &&
         request->sfc_base == mapped_base &&
         request->last_base == mapped_base &&
         request->check == h2_jieli_warm_request_check(mapped_base);
}

static inline uint32_t h2_jieli_warm_request_running_base(
    const volatile h2_jieli_warm_request_t *request, uint32_t mapped_base,
    int native_result, uint32_t native_base) {
  if (mapped_base != H2_JIELI_BANK_1_SFC_BASE &&
      mapped_base != H2_JIELI_BANK_2_SFC_BASE) return 0u;
  if (native_result == 0) return native_base == mapped_base ? mapped_base : 0u;
  return h2_jieli_warm_request_is_active_image(request, mapped_base)
      ? mapped_base : 0u;
}

/* Consume before attempting a hand-off. Interrupted publication, corruption,
 * and a second boot all fail closed into the ROM-selected Loader. */
static inline uint32_t h2_jieli_warm_request_take(
    volatile h2_jieli_warm_request_t *request) {
  const uint32_t magic = request->magic;
  const uint32_t base = request->sfc_base;
  const uint32_t check = request->check;
  request->magic = 0u;
  if (magic != H2_JIELI_WARM_MAGIC ||
      check != h2_jieli_warm_request_check(base) ||
      (base != H2_JIELI_BANK_1_SFC_BASE &&
       base != H2_JIELI_BANK_2_SFC_BASE)) {
    return 0u;
  }
  return base;
}

static inline void h2_jieli_warm_boot_request(uint32_t base) {
  volatile h2_jieli_warm_request_t *request =
      (volatile h2_jieli_warm_request_t *)H2_JIELI_WARM_REQUEST_ADDR;
  request->magic = 0u;
  request->sfc_base = base;
  request->check = h2_jieli_warm_request_check(base);
  request->magic = H2_JIELI_WARM_MAGIC;
}

#endif
