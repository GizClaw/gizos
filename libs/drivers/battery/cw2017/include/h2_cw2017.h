#ifndef H2_CW2017_H
#define H2_CW2017_H
#include "h2/pal/core/h2_pal_errors.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/** Borrowed, synchronous exact-byte register read. The caller owns I2C bus,
 * address, bounded timeout and serialization. Callback output is call-local.
 * Transport errors propagate unchanged. No allocation, locking or writes. */
typedef struct h2_cw2017_transport {
  void* user;
  h2_pal_result_t (*read_regs)(void* user, uint8_t reg, uint8_t* out,
                               size_t len);
} h2_cw2017_transport_t;

typedef struct h2_cw2017_sample {
  uint32_t voltage_uv;
  uint16_t soc_q8; /**< State of charge in 1/256 percent units, 0..25600. */
} h2_cw2017_sample_t;

/** Read version register 0x00. An ACK does not distinguish compatible chips.
 * Required output is cleared on failure. Call only in ordinary task context.
 * Returns INVALID_ARG, OK or the original transport error. */
h2_pal_result_t h2_cw2017_read_version(const h2_cw2017_transport_t* transport,
                                       uint8_t* out_version);
/** Read VCELL independently of SOC readiness. Output microvolts is cleared
 * on failure. Returns INVALID_ARG, OK or the original transport error.
 * Same borrowing, task-context and serialization contract as read_sample. */
h2_pal_result_t h2_cw2017_read_voltage(const h2_cw2017_transport_t* transport,
                                       uint32_t* out_voltage_uv);
/** Read voltage and SOC in two register transactions; not an atomic hardware
 * snapshot. Invalid/not-ready SOC returns WOULD_BLOCK and clears output.
 * The caller must configure and wake the gauge with its own battery profile
 * before sampling. No reset, profile programming or battery policy is implied.
 * Voltage rounds half microvolts down. Returns INVALID_ARG, WOULD_BLOCK, OK
 * or the original transport error. Required output is cleared on failure. */
h2_pal_result_t h2_cw2017_read_sample(const h2_cw2017_transport_t* transport,
                                      h2_cw2017_sample_t* out_sample);
#ifdef __cplusplus
}
#endif
#endif
