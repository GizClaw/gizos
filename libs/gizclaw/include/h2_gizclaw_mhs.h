#ifndef H2_GIZCLAW_MHS_H
#define H2_GIZCLAW_MHS_H

#include "h2/pal/core/h2_pal_errors.h"
#include "payload/mhs_v0.pb.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_GIZCLAW_MHS_ID_MAX_BYTES 64u

/** One typed HWD snapshot. Use the member selected by hwd. The generated
 * protobuf types preserve field presence, including measured false and zero. */
typedef union h2_gizclaw_mhs_read {
  gizclaw_rpc_v1_WifiHwdReadResponse wifi;
  gizclaw_rpc_v1_BleHwdReadResponse ble;
  gizclaw_rpc_v1_ModemHwdReadResponse modem;
  gizclaw_rpc_v1_BatteryHwdReadResponse battery;
  gizclaw_rpc_v1_MicHwdReadResponse mic;
  gizclaw_rpc_v1_DisplayHwdReadResponse display;
  gizclaw_rpc_v1_LedHwdReadResponse led;
  gizclaw_rpc_v1_SpeakerHwdReadResponse speaker;
} h2_gizclaw_mhs_read_t;

typedef union h2_gizclaw_mhs_write {
  gizclaw_rpc_v1_DisplayHwdWriteRequest display;
  gizclaw_rpc_v1_LedHwdWriteRequest led;
  gizclaw_rpc_v1_SpeakerHwdWriteRequest speaker;
} h2_gizclaw_mhs_write_t;

/** Device instance borrowed until Client/Service deinit.
 * The ID identifies a physical instance; hwd selects its protobuf shape.
 * read returns the current hardware snapshot. write applies only present
 * fields, then returns a fresh snapshot of what actually took effect.
 * Callbacks run on the RPC owner and must return promptly. A failed or timed
 * out write may already have changed hardware; callers must re-read.
 * RuntimeProfile manifest ownership is separate from device registration. */
typedef struct h2_gizclaw_mhs_device {
  const char *id;
  gizclaw_rpc_v1_ClientHwd hwd;
  h2_pal_result_t (*read)(void *user, h2_gizclaw_mhs_read_t *out);
  h2_pal_result_t (*write)(void *user, const h2_gizclaw_mhs_write_t *request,
                           h2_gizclaw_mhs_read_t *out_applied);
  void *user;
} h2_gizclaw_mhs_device_t;

#ifdef __cplusplus
}
#endif
#endif
