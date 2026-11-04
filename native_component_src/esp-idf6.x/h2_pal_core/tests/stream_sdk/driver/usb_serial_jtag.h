#ifndef STREAM_TEST_USB_JTAG_H
#define STREAM_TEST_USB_JTAG_H
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include <stdbool.h>
#include <stddef.h>
typedef struct {
  size_t rx_buffer_size, tx_buffer_size;
} usb_serial_jtag_driver_config_t;
bool usb_serial_jtag_is_driver_installed(void);
esp_err_t usb_serial_jtag_wait_tx_done(TickType_t ticks);
esp_err_t usb_serial_jtag_driver_uninstall(void);
esp_err_t usb_serial_jtag_driver_install(
    const usb_serial_jtag_driver_config_t* config);
void usb_serial_jtag_vfs_use_driver(void);
int usb_serial_jtag_read_bytes(void* data, size_t len, TickType_t ticks);
int usb_serial_jtag_write_bytes(const void* data, size_t len, TickType_t ticks);
#endif
