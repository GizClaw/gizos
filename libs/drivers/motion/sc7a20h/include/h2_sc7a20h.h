#ifndef H2_SC7A20H_H
#define H2_SC7A20H_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/os/h2_pal_mem.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_sc7a20h h2_sc7a20h_t;

typedef enum h2_sc7a20h_range {
    H2_SC7A20H_RANGE_2G = 0,
    H2_SC7A20H_RANGE_4G,
    H2_SC7A20H_RANGE_8G,
    H2_SC7A20H_RANGE_16G,
} h2_sc7a20h_range_t;

typedef enum h2_sc7a20h_odr {
    H2_SC7A20H_ODR_1_56HZ = 1,
    H2_SC7A20H_ODR_12_5HZ,
    H2_SC7A20H_ODR_25HZ,
    H2_SC7A20H_ODR_50HZ,
    H2_SC7A20H_ODR_100HZ,
    H2_SC7A20H_ODR_200HZ,
    H2_SC7A20H_ODR_400HZ,
    H2_SC7A20H_ODR_800HZ,
    H2_SC7A20H_ODR_1480HZ,
    H2_SC7A20H_ODR_2660HZ,
    H2_SC7A20H_ODR_4434HZ,
} h2_sc7a20h_odr_t;

/**
 * @brief Borrowed synchronous I2C transport, called only in task context.
 *
 * Callbacks must implement bounded, exact-byte transactions and propagate
 * failures. read_regs receives the wire sub-address unchanged: the driver sets
 * bit 7 for auto-increment burst reads. The caller owns the I2C address/bus and
 * must serialize all operations on an instance; this driver contains no lock.
 * user and callback dependencies must live until successful destroy().
 */
typedef struct h2_sc7a20h_transport {
    void *user;
    h2_pal_result_t (*write_reg)(void *user, uint8_t reg, uint8_t value);
    h2_pal_result_t (*read_regs)(void *user, uint8_t reg, uint8_t *out, size_t len);
    h2_pal_result_t (*sleep_ms)(void *user, uint32_t ms);
} h2_sc7a20h_transport_t;

/** Configuration is copied; mem and transport dependencies remain borrowed. */
typedef struct h2_sc7a20h_config {
    const h2_pal_mem_api_t *mem;
    h2_sc7a20h_transport_t transport;
    h2_sc7a20h_range_t range;
    h2_sc7a20h_odr_t odr;
    /** 0 disables INT1 routing; 1 routes data-ready, active-high/push-pull. */
    uint8_t data_ready_int1;
} h2_sc7a20h_config_t;

typedef struct h2_sc7a20h_sample {
    int32_t accel_mg[3];
} h2_sc7a20h_sample_t;

/**
 * @brief Allocate a closed instance without touching the sensor.
 * @param config Required valid allocator, transport, range and data rate.
 * @param out_device Required; cleared on failure, owns the returned instance.
 * @return OK, INVALID_ARG, or NO_MEMORY. Dependencies remain borrowed.
 */
h2_pal_result_t h2_sc7a20h_create(const h2_sc7a20h_config_t *config, h2_sc7a20h_t **out_device);

/**
 * @brief Verify WHO_AM_I=0x11 and VERSION=0x28 and start 12-bit high-performance
 * sampling with BDU, little-endian output and FIFO bypass.
 *
 * Idempotent when open. Blocks in the bounded transport plus a 1 ms settle
 * callback. A new sample may not yet be ready. Mismatched identity returns
 * NOT_FOUND without writes. A configuration failure attempts power-down and
 * remains closed; destroy/close can retry any failed cleanup.
 * @return OK, INVALID_ARG, NOT_FOUND, or the original transport/sleep failure.
 */
h2_pal_result_t h2_sc7a20h_open(h2_sc7a20h_t *device);

/**
 * @brief Read one complete, fresh XYZ sample rounded to nearest mg, using a
 * six-byte burst; half-mg ties round away from zero.
 * @param out_sample Required, cleared before validation and on every failure.
 * @return OK, INVALID_ARG, INVALID_STATE when closed, WOULD_BLOCK when XYZ is
 * not ready, or the exact transport failure. No gyro/magnetometer is provided.
 */
h2_pal_result_t h2_sc7a20h_read_sample(h2_sc7a20h_t *device, h2_sc7a20h_sample_t *out_sample);

/**
 * @brief Power down; idempotent when closed and cleanup is complete.
 * @return OK, INVALID_ARG for NULL, or a transport failure. A failed close
 * invalidates sampling and retains pending power-down so callers can retry
 * cleanup or reopen and configure the sensor again.
 */
h2_pal_result_t h2_sc7a20h_close(h2_sc7a20h_t *device);

/**
 * @brief Close and release the owned instance; NULL succeeds.
 * @return OK (pointer then invalid) or a close failure (pointer still owned).
 * Stop/wait for all callers before destruction; dependencies are not freed.
 */
h2_pal_result_t h2_sc7a20h_destroy(h2_sc7a20h_t *device);

#ifdef __cplusplus
}
#endif

#endif
