#include "h2_mosaico_usb_diagnostic.h"
#include "h2_mosaico_usb_transport.h"
const h2_pal_uart_io_stream_api_t* h2_mosaico_usb_diagnostic_api(void) {
  static const h2_pal_uart_io_stream_vtable_t vtable = {
      .write = h2_mosaico_usb_write, .flush = h2_mosaico_usb_flush};
  static const h2_pal_uart_io_stream_api_t api = {.vtable = &vtable};
  return &api;
}
