#ifndef H2_QRCODE_EXAMPLE_H
#define H2_QRCODE_EXAMPLE_H

#include "h2_qrcode.h"
#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Called once after the symbol is presented; the App forwards its result. */
typedef h2_pal_result_t (*h2_qrcode_example_ready_fn)(void *user);

typedef struct h2_qrcode_example_config {
  /** Borrowed NUL-terminated payload encoded into the symbol. */
  const char *text;
  /** Minimum error correction level. */
  h2_qrcode_ecc_t ecc;
  /** Largest version the encoder may select; 0 selects version 10. */
  int max_version;
  /** Quiet zone per side in modules; 0 selects the specified minimum. */
  int quiet_modules;
  /** Display brightness applied before drawing; 0 keeps the current level. */
  uint8_t brightness_percent;
  /** Opaque user pointer passed to `on_ready`. */
  void *on_ready_user;
  /** Optional callback invoked once after the symbol is presented. */
  h2_qrcode_example_ready_fn on_ready;
} h2_qrcode_example_config_t;

/**
 * @brief Encode the configured text and present it centered on the display.
 *
 * The call is blocking but does not loop: it draws the symbol once, presents
 * it, invokes `on_ready`, and returns. Module, scratch, and band storage come
 * from the Runtime memory PAL and are released before returning; the App keeps
 * no state between calls.
 *
 * @return H2_PAL_OK on success, H2_PAL_ERR_INVALID_ARG for a rejected
 *         argument, H2_PAL_ERR_UNAVAILABLE when the Runtime lacks the memory
 *         or Display capability, H2_PAL_ERR_NO_MEMORY when a buffer cannot be
 *         allocated, or the failing encode, layout, or Display PAL result.
 */
h2_pal_result_t
h2_qrcode_example_run(h2_runtime_t *runtime,
                      const h2_qrcode_example_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
