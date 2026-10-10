#ifndef H2_MOSAICO_USB_DIAGNOSTIC_H
#define H2_MOSAICO_USB_DIAGNOSTIC_H
#include "h2/pal/hal/h2_pal_uart_io_stream.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Output-only bounded CDC0 sink. Initialize the board USB console first.
 * Configure/read are unsupported; management CDC1 remains separate. */
const h2_pal_uart_io_stream_api_t *h2_mosaico_usb_diagnostic_api(void);
#ifdef __cplusplus
}
#endif
#endif
