#ifndef H2_MOSAICO_USB_CONSOLE_H
#define H2_MOSAICO_USB_CONSOLE_H
#ifdef __cplusplus
extern "C" {
#endif
/* Install the boot-lifetime USB CDC console before starting Runtime tasks.
 * Returns zero on success, a nonzero platform error on failure. Call exactly
 * once from serialized startup. No handles are borrowed from the caller. */
int h2_mosaico_usb_console_init(void);
#ifdef __cplusplus
}
#endif
#endif
