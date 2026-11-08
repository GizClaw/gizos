#ifdef NDEBUG
#undef NDEBUG
#endif
#include "h2_pal.h"
#include <assert.h>
#include <string.h>

static int calls;
static h2_pal_result_t mock_start(void* user) {
  assert(user == &calls);
  ++calls;
  return H2_PAL_OK;
}
static h2_pal_result_t mock_stop(void* user) {
  assert(user == &calls);
  ++calls;
  return H2_PAL_ERR_BUSY;
}
static h2_pal_result_t mock_acquire(void* user, h2_pal_camera_frame_t* frame) {
  assert(user == &calls && frame->data == NULL &&
         frame->format == H2_PAL_CAMERA_FORMAT_UNKNOWN);
  frame->width = 4;
  frame->height = 2;
  frame->stride = 8;
  frame->format = H2_PAL_CAMERA_FORMAT_UYVY;
  frame->token = 42;
  ++calls;
  return H2_PAL_OK;
}
static h2_pal_result_t mock_release(void* user,
                                    const h2_pal_camera_frame_t* frame) {
  assert(user == &calls && frame->token == 42);
  ++calls;
  return H2_PAL_ERR_TIMEOUT;
}

int main(void) {
  const h2_pal_camera_vtable_t empty_vtable = {0};
  const h2_pal_camera_api_t empty_api = {0};
  const h2_pal_camera_api_t missing_callbacks = {.vtable = &empty_vtable};
  const h2_pal_camera_api_t* apis[] = {
      NULL,
      &empty_api,
      &missing_callbacks,
      h2_pal_unsupported_camera_api(),
  };
  for (size_t i = 0; i < sizeof(apis) / sizeof(apis[0]); ++i) {
    h2_pal_camera_frame_t frame;
    memset(&frame, 0xff, sizeof(frame));
    assert(h2_pal_camera_start(apis[i]) == H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_camera_acquire(apis[i], &frame) == H2_PAL_ERR_UNSUPPORTED);
    assert(frame.data == NULL && frame.size == 0 && frame.width == 0 &&
           frame.height == 0 && frame.stride == 0 && frame.token == 0 &&
           frame.format == H2_PAL_CAMERA_FORMAT_UNKNOWN);
    assert(h2_pal_camera_acquire(apis[i], NULL) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_camera_release(apis[i], NULL) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_camera_release(apis[i], &frame) == H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_camera_stop(apis[i]) == H2_PAL_ERR_UNSUPPORTED);
  }
  const h2_pal_camera_vtable_t ops = {mock_start, mock_acquire, mock_release,
                                      mock_stop};
  const h2_pal_camera_api_t api = {.user = &calls, .vtable = &ops};
  h2_pal_camera_frame_t frame;
  memset(&frame, 0xff, sizeof(frame));
  assert(h2_pal_camera_start(&api) == H2_PAL_OK);
  assert(h2_pal_camera_acquire(&api, &frame) == H2_PAL_OK);
  assert(frame.format == H2_PAL_CAMERA_FORMAT_UYVY && frame.width == 4);
  assert(h2_pal_camera_release(&api, &frame) == H2_PAL_ERR_TIMEOUT);
  assert(frame.token ==
         42); /* Failed return leaves the caller's descriptor intact. */
  assert(h2_pal_camera_stop(&api) == H2_PAL_ERR_BUSY && calls == 4);
  return 0;
}
