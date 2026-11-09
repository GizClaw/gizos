#include "h2_jieli_wl82_console.h"

static int started;

static h2_pal_result_t stream_start(void* user) {
  (void)user;
  h2_pal_result_t rc = h2_jieli_wl82_console_start();
  if (rc == H2_PAL_OK) started = 1;
  return rc;
}

static h2_pal_result_t stream_read(void* user, void* buffer, size_t len,
                                   size_t* out_read, uint32_t timeout_ms) {
  if (!started) return H2_PAL_ERR_CLOSED;
  return h2_jieli_wl82_console_read(user, buffer, len, out_read, timeout_ms);
}

static h2_pal_result_t stream_write(void* user, const void* buffer, size_t len,
                                    size_t* out_written, uint32_t timeout_ms) {
  if (!started) return H2_PAL_ERR_CLOSED;
  h2_pal_result_t rc =
      h2_jieli_wl82_console_write(user, buffer, len, out_written, timeout_ms);
  /* The board UART may report an accepted prefix at its deadline. Its TX
   * lock is no longer held: report failure so a frame is never resumed after
   * another console producer has acquired the physical output. */
  if (rc == H2_PAL_OK && *out_written != len)
    return timeout_ms == 0u ? H2_PAL_ERR_WOULD_BLOCK : H2_PAL_ERR_TIMEOUT;
  return rc;
}

static h2_pal_result_t stream_flush(void* user) {
  return started ? h2_jieli_wl82_console_flush(user) : H2_PAL_ERR_CLOSED;
}

static h2_pal_result_t stream_release(void* user) {
  (void)user;
  /* CDC callbacks, RX worker and address-stable synchronization objects belong
   * to the native console for the entire boot, including app retries. */
  return H2_PAL_OK;
}

const h2_pal_io_stream_api_t* h2_jieli_wl82_platform_io_stream_api(void) {
  static const h2_pal_io_stream_vtable_t vtable = {
      .start = stream_start,
      .read = stream_read,
      .write = stream_write,
      .flush = stream_flush,
      .release = stream_release,
  };
  /* NULL selects the console's native boot-lifetime state. */
  static const h2_pal_io_stream_api_t api = {.vtable = &vtable};
  return &api;
}
