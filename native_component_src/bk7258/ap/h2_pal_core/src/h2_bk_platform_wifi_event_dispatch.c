#include <wdrv_cntrl.h>

#include <stddef.h>

void __real_wdrv_rx_handle_event(wdrv_rx_msg *message);

void __wrap_wdrv_rx_handle_event(wdrv_rx_msg *message) {
  if (message != NULL && message->id == BK_EVT_BCN_CC_RXED) {
    (void)bk_wifi_bcn_cc_rxed_cb(message->param, message->param_len);
    return;
  }
  __real_wdrv_rx_handle_event(message);
}
