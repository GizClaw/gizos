#ifndef H2_BM8563_TRANSPORT_H
#define H2_BM8563_TRANSPORT_H

#include "h2/pal/core/h2_pal_errors.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register transport for one BM8563/PCF8563-compatible device.
 *
 * Each callback performs one complete register access. Time/date accesses
 * from register 0x02 through 0x08 must remain one transaction and complete
 * within one second so the RTC's coherent-access guarantee remains valid.
 */
typedef struct h2_bm8563_transport_vtable {
    h2_pal_result_t (*read_registers)(void *user,
                                      uint8_t start_register,
                                      uint8_t *out_data,
                                      size_t data_length);
    h2_pal_result_t (*write_registers)(void *user,
                                       uint8_t start_register,
                                       const uint8_t *data,
                                       size_t data_length);
} h2_bm8563_transport_vtable_t;

typedef struct h2_bm8563_transport {
    void *user;
    const h2_bm8563_transport_vtable_t *vtable;
} h2_bm8563_transport_t;

#ifdef __cplusplus
}
#endif

#endif
