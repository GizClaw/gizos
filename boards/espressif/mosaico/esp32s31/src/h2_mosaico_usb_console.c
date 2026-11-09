#include "h2_mosaico_usb_console.h"
#include "h2_mosaico_usb_transport.h"
#ifdef H2_MOSAICO_USB_HOST_TEST
#include "h2_mosaico_usb_test_sdk.h"
#else
#include "esp_vfs.h"
#endif
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>

/* freopen opens this VFS path; a write-only device still needs open/close. */
static int console_open(const char* path, int flags, int mode) {
  (void)mode;
  if (strcmp(path, "/0") != 0) {
    errno = ENOENT;
    return -1;
  }
  if ((flags & O_ACCMODE) != O_WRONLY) {
    errno = EACCES;
    return -1;
  }
  return 0;
}
static int console_close(int fd) {
  (void)fd;
  return 0; /* The driver is owned for the boot lifetime. */
}
static ssize_t console_write(int fd, const void* data, size_t len) {
  (void)fd;
  size_t count = 0;
  int rc = h2_mosaico_usb_write(NULL, data, len, &count, 1000);
  if (rc != H2_PAL_OK) {
    errno = EIO;
    return -1;
  }
  return (ssize_t)count;
}
/* This VFS deliberately has no read callback: Loader is the sole RX owner. */
int h2_mosaico_usb_console_init(void) {
  static bool registered, ready;
  if (ready) return H2_PAL_OK;
  int rc = h2_mosaico_usb_common_init();
  if (rc != H2_PAL_OK) return rc;
  if (!registered) {
    const esp_vfs_t vfs = {.flags = ESP_VFS_FLAG_DEFAULT,
                           .open = console_open,
                           .close = console_close,
                           .write = console_write};
    if (esp_vfs_register("/dev/h2cdc", &vfs, NULL) != ESP_OK)
      return H2_PAL_ERR_IO;
    registered = true;
  }
  if (!freopen("/dev/h2cdc/0", "w", stdout)) return H2_PAL_ERR_IO;
  if (!freopen("/dev/h2cdc/0", "w", stderr)) return H2_PAL_ERR_IO;
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  ready = true;
  return H2_PAL_OK;
}
