#ifndef H2_ALIPAY_TAP_RADIO_H
#define H2_ALIPAY_TAP_RADIO_H

#include "hal/h2_pal_ble.h"
#include "os/h2_pal_mem.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_alipay_tap_radio h2_alipay_tap_radio_t;

typedef struct h2_alipay_tap_radio_config {
  const h2_pal_mem_api_t* memory;
  const h2_pal_ble_host_api_t* ble;
  uint8_t mac[6];
  void* user;
  void (*wake)(void* user);
} h2_alipay_tap_radio_config_t;

/** Register the 3802/4A02 payment service and start connectable advertising
 * with its MAC service data. Caller starts the BLE host first.
 * Configuration is copied; providers and wake context stay borrowed until
 * close succeeds. Open returns a retained object on partial cleanup failure;
 * callers must close any non-NULL output, even after an error. */
h2_pal_result_t h2_alipay_tap_radio_open(
    const h2_alipay_tap_radio_config_t* config, h2_alipay_tap_radio_t** out);

/** Allow writes after the current NFC input has been processed. No SDK
 * function runs on a GATT callback; at most eight copied 512-byte writes are
 * queued. Other peers, offsets and saturation return explicit ATT errors.
 * Caller serializes this operation with take/notify/close. */
h2_pal_result_t h2_alipay_tap_radio_arm(h2_alipay_tap_radio_t* radio);

/** Copy and consume the next accepted terminal write, or WOULD_BLOCK.
 * Short buffers preserve the queued write and report its required length.
 * One peer owns the session until close; no automatic peer replacement. */
h2_pal_result_t h2_alipay_tap_radio_take(h2_alipay_tap_radio_t* radio,
                                         uint8_t* out, size_t* length);

/** Submit a copied SDK Notify fragment to the current peer. Uses the
 * universally supported 20-byte ATT payload. A final successful submission
 * seals input for this attempt; transport success is not payment evidence. */
h2_pal_result_t h2_alipay_tap_radio_notify(h2_alipay_tap_radio_t* radio,
                                           bool finish, const uint8_t* data,
                                           size_t length);

/** Worker-only cleanup. Stops its advertising, unregisters only its own
 * service and disconnects its peer. Failure retains all unfinished ownership
 * for retry. Callbacks are detached before memory is wiped/freed. */
h2_pal_result_t h2_alipay_tap_radio_close(h2_alipay_tap_radio_t** radio);

#ifdef __cplusplus
}
#endif
#endif
