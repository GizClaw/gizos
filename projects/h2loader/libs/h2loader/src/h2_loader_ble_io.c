#include "h2_loader_ble.h"
#include "h2_loader_app_client.h"

#include <stdio.h>

#define H2_LOADER_BLE_IO_TIMEOUT_MS 5000u

static h2_pal_result_t command_read(void* user, void* buffer, size_t len,
                                    size_t* out_read, uint32_t timeout_ms) {
  return (h2_pal_result_t)h2_bleikcp_read(user, buffer, len, out_read,
                                          timeout_ms);
}

static h2_pal_result_t command_write(void* user, const void* buffer, size_t len,
                                     size_t* out_written, uint32_t timeout_ms) {
  if (out_written == NULL || (len > 0u && buffer == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  *out_written = 0u;
  size_t chunk = len;
  int rc;
  /* Command I/O permits short writes; a small ring must not truncate a reply. */
  for (;;) {
    rc = h2_bleikcp_write(user, buffer, chunk, timeout_ms);
    if (rc != H2_PAL_ERR_NO_SPACE || chunk <= 1u) break;
    chunk /= 2u;
  }
  if (rc == H2_PAL_OK) *out_written = chunk;
  return rc;
}

static h2_pal_result_t command_flush(void* user) {
  return (h2_pal_result_t)h2_bleikcp_flush(user, H2_LOADER_BLE_IO_TIMEOUT_MS);
}

h2_command_io_api_t h2_loader_ble_command_io(h2_bleikcp_t* stream) {
  static const h2_command_io_vtable_t vtable = {
      .read = command_read,
      .write = command_write,
      .flush = command_flush,
  };
  return (h2_command_io_api_t){
      .user = stream,
      .vtable = stream != NULL ? &vtable : NULL,
  };
}

int h2_loader_ble_app_read_byte(void* user, uint32_t timeout_ms) {
  uint8_t value = 0u;
  size_t count = 0u;
  int rc = h2_bleikcp_read(user, &value, sizeof(value), &count, timeout_ms);
  if (rc == H2_PAL_OK && count == 1u) return value;
  if (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK ||
      (rc == H2_PAL_OK && count == 0u))
    return EOF;
  return H2_LOADER_APP_CLIENT_SESSION_CLOSED;
}

int h2_loader_ble_app_write(void* user, const char* data, size_t len) {
  if (data == NULL && len > 0u) return H2_PAL_ERR_INVALID_ARG;
  size_t sent = 0u;
  while (sent < len) {
    size_t written = 0u;
    int rc = command_write(user, data + sent, len - sent, &written,
                            H2_LOADER_BLE_IO_TIMEOUT_MS);
    if (rc != H2_PAL_OK) return rc;
    if (written == 0u) return H2_PAL_ERR_IO;
    sent += written;
  }
  return h2_bleikcp_flush(user, H2_LOADER_BLE_IO_TIMEOUT_MS);
}
