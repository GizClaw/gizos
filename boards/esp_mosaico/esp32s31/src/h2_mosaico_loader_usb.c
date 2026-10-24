#include "h2_mosaico_loader_usb.h"
#include "h2_mosaico_usb_common.h"
#include "h2_esp_h2loader_iostreamikcp.h"
int h2_mosaico_loader_usb_init(void) {
  const h2_iostreamikcp_io_t io = {.read = h2_mosaico_usb_read,
                                   .write = h2_mosaico_usb_write,
                                   .flush = h2_mosaico_usb_flush};
  /* Caller initializes board USB/console first. Injection owns no SDK state. */
  return h2_esp_h2loader_configure_physical_io(&io);
}
