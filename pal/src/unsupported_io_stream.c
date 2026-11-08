#include "h2_pal.h"

static h2_pal_result_t unsupported_lifecycle(void* user) {
  (void)user;
  return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_read(void* user, void* buffer, size_t len,
                                        size_t* out_read, uint32_t timeout_ms) {
  (void)user;
  (void)buffer;
  (void)len;
  (void)timeout_ms;
  if (out_read != NULL) *out_read = 0u;
  return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_write(void* user, const void* buffer,
                                         size_t len, size_t* out_written,
                                         uint32_t timeout_ms) {
  (void)user;
  (void)buffer;
  (void)len;
  (void)timeout_ms;
  if (out_written != NULL) *out_written = 0u;
  return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_io_stream_vtable_t vtable = {
    .start = unsupported_lifecycle,
    .read = unsupported_read,
    .write = unsupported_write,
    .flush = unsupported_lifecycle,
    .release = unsupported_lifecycle,
};
static const h2_pal_io_stream_api_t api = {.vtable = &vtable};

const h2_pal_io_stream_api_t* h2_pal_unsupported_io_stream_api(void) {
  return &api;
}
