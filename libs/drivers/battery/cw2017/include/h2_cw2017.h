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
  uint32_t voltage_uv; /**< Microvolts; floor((VCELL & 0x3fff) * 625 / 2). */
  uint16_t soc_q8; /**< Exact 1/256 percent units, inclusive range 0..25600. */
} h2_cw2017_sample_t;

/** Read version register 0x00. An ACK does not distinguish compatible chips.
 * Required output is cleared on failure. Call only in ordinary task context.
 * Returns INVALID_ARG, OK or the original transport error. */
h2_pal_result_t h2_cw2017_read_version(const h2_cw2017_transport_t* transport,
                                       uint8_t* out_version);
/** Read the low 14 bits of big-endian VCELL at 0x02, independently of SOC.
 * Convert 312.5 microvolts/count to integer microvolts, rounding down.
 * Output is cleared on failure. Returns INVALID_ARG, OK or the original
 * transport error. Same borrowing and task-context contract as read_sample. */
h2_pal_result_t h2_cw2017_read_voltage(const h2_cw2017_transport_t* transport,
                                       uint32_t* out_voltage_uv);
/** Read voltage and SOC in two register transactions; not an atomic hardware
 * snapshot. SOC at 0x04 is big-endian Q8 percent, returned without rounding.
 * The inclusive 0..25600 range represents 0..100 percent. Greater values return
 * WOULD_BLOCK and clear both sample fields, including the valid voltage.
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
