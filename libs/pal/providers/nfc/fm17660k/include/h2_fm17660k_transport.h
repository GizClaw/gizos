#ifndef H2_FM17660K_TRANSPORT_H
#define H2_FM17660K_TRANSPORT_H

#include "h2/pal/core/h2_pal_errors.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_fm17660k_transport_vtable {
    h2_pal_result_t (*reset)(void *user);
    h2_pal_result_t (*write_reg)(void *user, uint8_t reg, uint8_t value);
    /** Write every byte through the same chip register (for example FIFO). */
    h2_pal_result_t (*write_regs)(
        void *user, uint8_t reg, const uint8_t *data, size_t len);
    h2_pal_result_t (*read_reg)(
        void *user, uint8_t reg, uint8_t *out_value);
    /** Read every byte through the same chip register (for example FIFO). */
    h2_pal_result_t (*read_regs)(
        void *user, uint8_t reg, uint8_t *out_data, size_t len);
    uint64_t (*now_ms)(void *user);
    h2_pal_result_t (*sleep_ms)(void *user, uint32_t delay_ms);
} h2_fm17660k_transport_vtable_t;

typedef struct h2_fm17660k_transport {
    void *user;
    const h2_fm17660k_transport_vtable_t *vtable;
} h2_fm17660k_transport_t;

typedef struct h2_fm17660k_byte_endpoint {
    void *user;
    h2_pal_result_t (*write_exact)(
        void *user, const uint8_t *data, size_t len, uint32_t timeout_ms);
    h2_pal_result_t (*read_exact)(
        void *user, uint8_t *data, size_t len, uint32_t timeout_ms);
} h2_fm17660k_byte_endpoint_t;

typedef struct h2_fm17660k_byte_transport_config {
    h2_fm17660k_byte_endpoint_t endpoint;
    h2_pal_result_t (*reset)(void *user);
    void *reset_user;
    uint64_t (*now_ms)(void *user);
    h2_pal_result_t (*sleep_ms)(void *user, uint32_t delay_ms);
    void *clock_user;
    uint32_t operation_timeout_ms;
} h2_fm17660k_byte_transport_config_t;

/**
 * Construct an exact register transport over a borrowed exact-byte endpoint.
 *
 * The config and everything it references remain borrowed until the driver
 * using the returned transport is deinitialized.
 */
h2_pal_result_t h2_fm17660k_transport_from_byte_endpoint(
    const h2_fm17660k_byte_transport_config_t *config,
    h2_fm17660k_transport_t *out_transport);

#ifdef __cplusplus
}
#endif

#endif
