#define _POSIX_C_SOURCE 200809L
#include "../src/h2_esp_board_private.h"
#include "esp_heap_caps.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

static int bus, bus_available = 1, fd_open, streaming, video_owned;
static esp_err_t init_error, deinit_error;
static int open_error, close_error;
static unsigned long ioctl_error;
static unsigned allocations, frees, fail_alloc_after, dequeues, queued[2];
static unsigned init_calls, malformed, index_error, overlap;
static bool external_ram = true;

i2c_master_bus_handle_t h2_esp_p4_board_i2c_bus(void) {
  return bus_available ? &bus : NULL;
}
esp_err_t esp_video_init_with_flags(const esp_video_init_config_t* config,
                                    uint32_t flags) {
  assert(flags == (ESP_VIDEO_INIT_FLAGS_MIPI_CSI | ESP_VIDEO_INIT_FLAGS_ISP));
  assert(!video_owned && !config->csi->sccb_config.init_sccb &&
         config->csi->sccb_config.i2c_handle == &bus &&
         config->csi->sccb_config.freq == 100000 &&
         config->csi->reset_pin == -1 && config->csi->pwdn_pin == -1);
  ++init_calls;
  if (init_error != ESP_OK) return init_error; /* SDK already unwound. */
  video_owned = 1;
  return ESP_OK;
}
esp_err_t esp_video_deinit_with_flags(uint32_t flags) {
  assert(flags == (ESP_VIDEO_INIT_FLAGS_MIPI_CSI | ESP_VIDEO_INIT_FLAGS_ISP));
  assert(!streaming && !fd_open);
  if (deinit_error != ESP_OK) return deinit_error;
  video_owned = 0;
  return ESP_OK;
}
int h2_p4_test_open(const char* name, int flags, ...) {
  assert(strcmp(name, "/dev/video0") == 0 && flags == O_RDONLY);
  assert(video_owned && !fd_open);
  if (open_error) {
    errno = open_error;
    return -1;
  }
  fd_open = 1;
  return 42;
}
int h2_p4_test_close(int fd) {
  assert(fd == 42 && fd_open && !streaming);
  if (close_error) {
    errno = close_error;
    return -1;
  }
  fd_open = 0;
  queued[0] = queued[1] = 0;
  return 0;
}
int h2_p4_test_ioctl(int fd, unsigned long request, ...) {
  assert(fd == 42 && fd_open);
  va_list args;
  va_start(args, request);
  void* raw = va_arg(args, void*);
  va_end(args);
  if (ioctl_error == request) {
    errno = EIO;
    return -1;
  }
  switch (request) {
    case VIDIOC_G_FMT: {
      struct v4l2_format* f = raw;
      f->fmt.pix.width = 8;
      f->fmt.pix.height = 6;
      f->fmt.pix.bytesperline = 0; /* Driver's packed format convention. */
      f->fmt.pix.pixelformat = V4L2_PIX_FMT_UYVY;
      break;
    }
    case VIDIOC_S_FMT: {
      struct v4l2_format* f = raw;
      assert(f->fmt.pix.width == 8 && f->fmt.pix.height == 6 &&
             f->fmt.pix.pixelformat == V4L2_PIX_FMT_UYVY &&
             f->fmt.pix.quantization == V4L2_QUANTIZATION_LIM_RANGE &&
             f->fmt.pix.ycbcr_enc == V4L2_YCBCR_ENC_601);
      break;
    }
    case VIDIOC_S_DQBUF_TIMEOUT:
      assert(((struct timeval*)raw)->tv_sec == 1 &&
             ((struct timeval*)raw)->tv_usec == 0);
      break;
    case VIDIOC_REQBUFS: {
      struct v4l2_requestbuffers* r = raw;
      assert(r->count == 2 && r->memory == V4L2_MEMORY_USERPTR &&
             r->type == V4L2_BUF_TYPE_VIDEO_CAPTURE);
      break;
    }
    case VIDIOC_QUERYBUF:
      ((struct v4l2_buffer*)raw)->length = 96;
      break;
    case VIDIOC_QBUF: {
      struct v4l2_buffer* b = raw;
      assert(b->index < 2 && b->m.userptr && b->length == 96 &&
             !queued[b->index]);
      queued[b->index] = 1;
      break;
    }
    case VIDIOC_DQBUF: {
      assert(streaming);
      if (overlap) {
        h2_pal_camera_frame_t other;
        assert(h2_pal_camera_stop(h2_esp_board_camera_api()) ==
               H2_PAL_ERR_BUSY);
        assert(h2_pal_camera_acquire(h2_esp_board_camera_api(), &other) ==
               H2_PAL_ERR_BUSY);
        overlap = 0;
      }
      struct v4l2_buffer* b = raw;
      b->index = index_error ? 2 : dequeues++ % 2;
      if (b->index < 2) {
        assert(queued[b->index]);
        queued[b->index] = 0;
      }
      b->bytesused = malformed ? 95 : 96;
      break;
    }
    case VIDIOC_STREAMON:
      assert(!streaming && queued[0] && queued[1]);
      streaming = 1;
      dequeues = 0;
      break;
    case VIDIOC_STREAMOFF:
      assert(streaming);
      streaming = 0;
      break;
    default:
      assert(0);
  }
  return 0;
}
void* heap_caps_aligned_alloc(size_t alignment, size_t size, unsigned caps) {
  assert(alignment == 128 && size == 96 &&
         caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (fail_alloc_after && --fail_alloc_after == 0) return NULL;
  void* p = NULL;
  assert(posix_memalign(&p, alignment, size) == 0);
  ++allocations;
  return p;
}
void heap_caps_free(void* ptr) {
  if (ptr) {
    assert(!fd_open && !streaming && !video_owned);
    ++frees;
    free(ptr);
  }
}
bool esp_ptr_external_ram(const void* ptr) {
  assert(ptr != NULL);
  return external_ram;
}
static void clean(const h2_pal_camera_api_t* api) {
  assert(h2_pal_camera_stop(api) == 0);
  assert(!fd_open && !streaming && !video_owned && allocations == frees);
}
int main(void) {
  const h2_pal_camera_api_t* api = h2_esp_board_camera_api();
  h2_pal_camera_frame_t frame, other;
  clean(api);
  assert(h2_pal_camera_acquire(api, &frame) == H2_PAL_ERR_INVALID_STATE);
  bus_available = 0;
  assert(h2_pal_camera_start(api) == H2_PAL_ERR_UNAVAILABLE);
  bus_available = 1;
  init_error = ESP_ERR_TIMEOUT;
  assert(h2_pal_camera_start(api) == H2_PAL_ERR_TIMEOUT);
  clean(api);
  init_error = ESP_OK;
  open_error = ENODEV;
  assert(h2_pal_camera_start(api) == H2_PAL_ERR_NOT_FOUND);
  clean(api);
  open_error = 0;
  ioctl_error = VIDIOC_S_FMT;
  assert(h2_pal_camera_start(api) == H2_PAL_ERR_IO);
  clean(api);
  ioctl_error = 0;
  fail_alloc_after = 2;
  assert(h2_pal_camera_start(api) == H2_PAL_ERR_NO_MEMORY);
  clean(api);
  external_ram = false;
  assert(h2_pal_camera_start(api) == H2_PAL_ERR_FORMAT);
  clean(api);
  external_ram = true;
  assert(h2_pal_camera_start(api) == 0);
  unsigned calls = init_calls;
  assert(h2_pal_camera_start(api) == 0 && init_calls == calls);
  overlap = 1;
  assert(h2_pal_camera_acquire(api, &frame) == 0);
  assert(frame.width == 8 && frame.height == 6 && frame.stride == 16 &&
         frame.size == 96 && frame.token != 0 &&
         frame.format == H2_PAL_CAMERA_FORMAT_UYVY);
  assert(h2_pal_camera_acquire(api, &other) == H2_PAL_ERR_BUSY);
  assert(other.data == NULL && other.token == 0);
  assert(h2_pal_camera_stop(api) == H2_PAL_ERR_BUSY);
  other = frame;
  ++other.token;
  assert(h2_pal_camera_release(api, &other) == H2_PAL_ERR_INVALID_STATE);
  other = frame;
  ++other.stride;
  assert(h2_pal_camera_release(api, &other) == H2_PAL_ERR_INVALID_STATE);
  ioctl_error = VIDIOC_QBUF;
  assert(h2_pal_camera_release(api, &frame) == H2_PAL_ERR_IO);
  assert(h2_pal_camera_stop(api) == H2_PAL_ERR_BUSY);
  ioctl_error = 0;
  assert(h2_pal_camera_release(api, &frame) == 0);
  assert(h2_pal_camera_release(api, &frame) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_pal_camera_acquire(api, &other) == 0);
  assert(other.token != frame.token);
  assert(h2_pal_camera_release(api, &frame) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_pal_camera_release(api, &other) == 0);
  malformed = 1;
  ioctl_error = VIDIOC_QBUF;
  assert(h2_pal_camera_acquire(api, &other) == H2_PAL_ERR_IO);
  assert(other.data == NULL);
  assert(h2_pal_camera_stop(api) == H2_PAL_ERR_IO);
  ioctl_error = 0;
  malformed = 0;
  clean(api);
  assert(h2_pal_camera_start(api) == 0);
  ioctl_error = VIDIOC_DQBUF;
  assert(h2_pal_camera_acquire(api, &other) == H2_PAL_ERR_IO);
  ioctl_error = VIDIOC_STREAMOFF;
  assert(h2_pal_camera_stop(api) == H2_PAL_ERR_IO);
  assert(h2_pal_camera_acquire(api, &other) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_pal_camera_start(api) == H2_PAL_ERR_BUSY);
  ioctl_error = 0;
  close_error = EIO;
  assert(h2_pal_camera_stop(api) == H2_PAL_ERR_IO);
  close_error = 0;
  deinit_error = ESP_FAIL;
  assert(h2_pal_camera_stop(api) == H2_PAL_ERR_IO);
  assert(h2_pal_camera_start(api) == H2_PAL_ERR_BUSY);
  deinit_error = ESP_OK;
  clean(api);
  assert(h2_pal_camera_start(api) == 0);
  index_error = 1;
  assert(h2_pal_camera_acquire(api, &frame) == H2_PAL_ERR_FORMAT);
  index_error = 0;
  clean(api);
  assert(h2_pal_camera_start(api) == 0);
  assert(h2_pal_camera_acquire(api, &frame) == 0);
  assert(h2_pal_camera_release(api, &frame) == 0);
  clean(api);
  return 0;
}
