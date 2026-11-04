#include "h2_pal.h"

static h2_pal_result_t unsupported_image_get_info(
    void* user, uint32_t partition_id, h2_pal_image_view_t view,
    h2_pal_image_info_t* out_info) {
  (void)user;
  (void)partition_id;
  (void)view;
  (void)out_info;
  return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_image_read(void* user, uint32_t partition_id,
                                              h2_pal_image_view_t view,
                                              uint64_t offset, void* data,
                                              size_t len) {
  (void)user;
  (void)partition_id;
  (void)view;
  (void)offset;
  (void)data;
  (void)len;
  return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_image_begin(
    void* user, const h2_pal_image_write_request_t* request) {
  (void)user;
  (void)request;
  return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_image_write(void* user, const void* data,
                                               size_t len) {
  (void)user;
  (void)data;
  (void)len;
  return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_image_end(void* user) {
  (void)user;
  return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_image_vtable_t unsupported_image_vtable = {
    .get_info = unsupported_image_get_info,
    .read = unsupported_image_read,
    .begin = unsupported_image_begin,
    .write = unsupported_image_write,
    .finish = unsupported_image_end,
    .abort = unsupported_image_end,
};
static const h2_pal_image_api_t unsupported_image_api = {
    .user = NULL,
    .vtable = &unsupported_image_vtable,
};

const h2_pal_image_api_t* h2_pal_unsupported_image_api(void) {
  return &unsupported_image_api;
}
