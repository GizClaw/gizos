#ifndef H2_QMI8658_TRANSPORT_H
#define H2_QMI8658_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#include "h2/pal/core/h2_pal_errors.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_qmi8658_transport {
    void *user;
    h2_pal_result_t (*write_reg)(void *user, uint8_t reg, uint8_t value);
    h2_pal_result_t (*read_regs)(void *user, uint8_t reg, uint8_t *out, size_t len);
    void (*sleep_ms)(void *user, uint32_t ms);
} h2_qmi8658_transport_t;

#ifdef __cplusplus
}
#endif

#endif
