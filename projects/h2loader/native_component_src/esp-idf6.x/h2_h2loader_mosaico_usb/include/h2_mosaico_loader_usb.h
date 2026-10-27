#ifndef H2_MOSAICO_LOADER_USB_H
#define H2_MOSAICO_LOADER_USB_H

/* Bind H2Loader to the shared CDC0 at serialized startup; it is the sole RX
 * owner. */
int h2_mosaico_loader_usb_init(void);

#endif
