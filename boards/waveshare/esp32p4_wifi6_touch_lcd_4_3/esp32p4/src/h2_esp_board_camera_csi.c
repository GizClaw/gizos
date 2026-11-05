#include "h2_esp_board_private.h"
#include "h2_esp_p4_board_internal.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

enum { BUFFER_COUNT = 2, BUFFER_ALIGNMENT = 128 };
#define VIDEO_FLAGS (ESP_VIDEO_INIT_FLAGS_MIPI_CSI | ESP_VIDEO_INIT_FLAGS_ISP)

static struct {
  int fd;
  bool video_owned, streaming, running, pending, loaned;
  void* buffers[BUFFER_COUNT];
  size_t lengths[BUFFER_COUNT];
  struct v4l2_buffer borrowed;
  uint32_t width, height, stride;
  uint64_t token;
} s_camera = {.fd = -1};
static atomic_flag s_operation = ATOMIC_FLAG_INIT;

static h2_pal_result_t camera_result(esp_err_t err) {
  if (err == ESP_OK) return H2_PAL_OK;
  if (err == ESP_ERR_NO_MEM) return H2_PAL_ERR_NO_MEMORY;
  if (err == ESP_ERR_TIMEOUT) return H2_PAL_ERR_TIMEOUT;
  if (err == ESP_ERR_NOT_FOUND) return H2_PAL_ERR_NOT_FOUND;
  if (err == ESP_ERR_INVALID_ARG) return H2_PAL_ERR_INVALID_ARG;
  if (err == ESP_ERR_INVALID_STATE) return H2_PAL_ERR_INVALID_STATE;
  if (err == ESP_ERR_NOT_SUPPORTED) return H2_PAL_ERR_UNSUPPORTED;
  return H2_PAL_ERR_IO;
}

static h2_pal_result_t camera_errno(void) {
  if (errno == ENOMEM) return H2_PAL_ERR_NO_MEMORY;
  if (errno == ETIMEDOUT) return H2_PAL_ERR_TIMEOUT;
  if (errno == EAGAIN) return H2_PAL_ERR_WOULD_BLOCK;
  if (errno == ENOENT || errno == ENODEV) return H2_PAL_ERR_NOT_FOUND;
  if (errno == EINVAL) return H2_PAL_ERR_INVALID_ARG;
  if (errno == EBUSY) return H2_PAL_ERR_BUSY;
  if (errno == ENOSYS || errno == ENOTSUP || errno == ESRCH)
    return H2_PAL_ERR_UNSUPPORTED;
  return H2_PAL_ERR_IO;
}

static h2_pal_result_t return_pending(void) {
  if (!s_camera.pending) return H2_PAL_OK;
  if (ioctl(s_camera.fd, VIDIOC_QBUF, &s_camera.borrowed) != 0)
    return camera_errno();
  s_camera.pending = false;
  s_camera.loaned = false;
  memset(&s_camera.borrowed, 0, sizeof(s_camera.borrowed));
  return H2_PAL_OK;
}

static h2_pal_result_t stop_impl(void) {
  if (s_camera.loaned) return H2_PAL_ERR_BUSY;
  s_camera.running = false;
  h2_pal_result_t rc = return_pending();
  if (rc != H2_PAL_OK) return rc;
  if (s_camera.streaming) {
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_camera.fd, VIDIOC_STREAMOFF, &type) != 0) return camera_errno();
    s_camera.streaming = false;
  }
  if (s_camera.fd >= 0) {
    if (close(s_camera.fd) != 0) return camera_errno();
    s_camera.fd = -1;
  }
  if (s_camera.video_owned) {
    rc = camera_result(esp_video_deinit_with_flags(VIDEO_FLAGS));
    if (rc != H2_PAL_OK) return rc;
    s_camera.video_owned = false;
  }
  for (unsigned i = 0; i < BUFFER_COUNT; ++i) {
    heap_caps_free(s_camera.buffers[i]);
    s_camera.buffers[i] = NULL;
    s_camera.lengths[i] = 0;
  }
  s_camera.width = s_camera.height = s_camera.stride = 0;
  return H2_PAL_OK;
}

static h2_pal_result_t start_impl(void) {
  if (s_camera.running) return H2_PAL_OK;
  if (s_camera.video_owned || s_camera.fd >= 0) return H2_PAL_ERR_BUSY;
  i2c_master_bus_handle_t bus = h2_esp_p4_board_i2c_bus();
  if (bus == NULL) return H2_PAL_ERR_UNAVAILABLE;
  const esp_video_init_csi_config_t csi = {
      .sccb_config = {.init_sccb = false, .i2c_handle = bus, .freq = 100000},
      .reset_pin = -1,
      .pwdn_pin = -1,
  };
  const esp_video_init_config_t config = {.csi = &csi};
  /* The flag-based SDK cleanup is idempotent, including failed init unwind. */
  s_camera.video_owned = true;
  h2_pal_result_t rc =
      camera_result(esp_video_init_with_flags(&config, VIDEO_FLAGS));
  if (rc != H2_PAL_OK) goto fail;
  s_camera.fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
  if (s_camera.fd < 0) {
    rc = camera_errno();
    goto fail;
  }
  struct v4l2_format format = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
  if (ioctl(s_camera.fd, VIDIOC_G_FMT, &format) != 0) {
    rc = camera_errno();
    goto fail;
  }
  format.fmt.pix.pixelformat = V4L2_PIX_FMT_UYVY;
  /* Match MFG's BT.601 limited-range RGB conversion. */
  format.fmt.pix.quantization = V4L2_QUANTIZATION_LIM_RANGE;
  format.fmt.pix.ycbcr_enc = V4L2_YCBCR_ENC_601;
  if (ioctl(s_camera.fd, VIDIOC_S_FMT, &format) != 0 ||
      ioctl(s_camera.fd, VIDIOC_G_FMT, &format) != 0) {
    rc = camera_errno();
    goto fail;
  }
  s_camera.width = format.fmt.pix.width;
  s_camera.height = format.fmt.pix.height;
  s_camera.stride = format.fmt.pix.bytesperline;
  if (s_camera.width < 2 || s_camera.width > 4096 || (s_camera.width & 1) ||
      s_camera.height == 0 || s_camera.height > 4096 ||
      format.fmt.pix.pixelformat != V4L2_PIX_FMT_UYVY) {
    rc = H2_PAL_ERR_FORMAT;
    goto fail;
  }
  if (s_camera.stride == 0) s_camera.stride = s_camera.width * 2;
  if (s_camera.stride < s_camera.width * 2 ||
      s_camera.height > SIZE_MAX / s_camera.stride) {
    rc = H2_PAL_ERR_FORMAT;
    goto fail;
  }
  const struct timeval timeout = {.tv_sec = 1};
  if (ioctl(s_camera.fd, VIDIOC_S_DQBUF_TIMEOUT, &timeout) != 0) {
    rc = camera_errno();
    goto fail;
  }
  struct v4l2_requestbuffers request = {.count = BUFFER_COUNT,
                                        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                                        .memory = V4L2_MEMORY_USERPTR};
  if (ioctl(s_camera.fd, VIDIOC_REQBUFS, &request) != 0) {
    rc = camera_errno();
    goto fail;
  }
  if (request.count != BUFFER_COUNT) {
    rc = H2_PAL_ERR_FORMAT;
    goto fail;
  }
  for (unsigned i = 0; i < BUFFER_COUNT; ++i) {
    struct v4l2_buffer buffer = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                                 .memory = V4L2_MEMORY_USERPTR,
                                 .index = i};
    if (ioctl(s_camera.fd, VIDIOC_QUERYBUF, &buffer) != 0) {
      rc = camera_errno();
      goto fail;
    }
    if (buffer.length < (size_t)s_camera.stride * s_camera.height) {
      rc = H2_PAL_ERR_FORMAT;
      goto fail;
    }
    s_camera.lengths[i] = buffer.length;
    s_camera.buffers[i] = heap_caps_aligned_alloc(
        BUFFER_ALIGNMENT, buffer.length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_camera.buffers[i] == NULL) {
      rc = H2_PAL_ERR_NO_MEMORY;
      goto fail;
    }
    if (!esp_ptr_external_ram(s_camera.buffers[i])) {
      rc = H2_PAL_ERR_FORMAT;
      goto fail;
    }
    buffer.m.userptr = (unsigned long)s_camera.buffers[i];
    if (ioctl(s_camera.fd, VIDIOC_QBUF, &buffer) != 0) {
      rc = camera_errno();
      goto fail;
    }
  }
  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(s_camera.fd, VIDIOC_STREAMON, &type) != 0) {
    rc = camera_errno();
    goto fail;
  }
  s_camera.streaming = s_camera.running = true;
  ESP_LOGI("h2_p4_camera",
           "CSI ready UYVY %lux%lu stride=%lu buffers=2 psram_bytes=%zu",
           (unsigned long)s_camera.width, (unsigned long)s_camera.height,
           (unsigned long)s_camera.stride,
           s_camera.lengths[0] + s_camera.lengths[1]);
  return H2_PAL_OK;
fail: {
  h2_pal_result_t cleanup = stop_impl();
  return cleanup == H2_PAL_OK ? rc : cleanup;
}
}

static h2_pal_result_t acquire_impl(h2_pal_camera_frame_t* out) {
  if (!s_camera.running) return H2_PAL_ERR_INVALID_STATE;
  if (s_camera.pending) return H2_PAL_ERR_BUSY;
  struct v4l2_buffer buffer = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                               .memory = V4L2_MEMORY_USERPTR};
  if (ioctl(s_camera.fd, VIDIOC_DQBUF, &buffer) != 0) return camera_errno();
  if (buffer.index >= BUFFER_COUNT) return H2_PAL_ERR_FORMAT;
  buffer.m.userptr = (unsigned long)s_camera.buffers[buffer.index];
  buffer.length = s_camera.lengths[buffer.index];
  s_camera.borrowed = buffer;
  s_camera.pending = true;
  if ((buffer.flags & V4L2_BUF_FLAG_ERROR) ||
      buffer.bytesused < (size_t)s_camera.stride * s_camera.height ||
      buffer.bytesused > buffer.length ||
      !esp_ptr_external_ram(s_camera.buffers[buffer.index])) {
    h2_pal_result_t rc = return_pending();
    return rc == H2_PAL_OK ? H2_PAL_ERR_FORMAT : rc;
  }
  s_camera.loaned = true;
  if (++s_camera.token == 0) ++s_camera.token;
  *out = (h2_pal_camera_frame_t){.data = s_camera.buffers[buffer.index],
                                 .size = buffer.bytesused,
                                 .width = s_camera.width,
                                 .height = s_camera.height,
                                 .stride = s_camera.stride,
                                 .token = s_camera.token,
                                 .format = H2_PAL_CAMERA_FORMAT_UYVY};
  return H2_PAL_OK;
}

static h2_pal_result_t release_impl(const h2_pal_camera_frame_t* frame) {
  if (!s_camera.loaned || frame->token != s_camera.token ||
      frame->data != s_camera.buffers[s_camera.borrowed.index] ||
      frame->size != s_camera.borrowed.bytesused ||
      frame->width != s_camera.width || frame->height != s_camera.height ||
      frame->stride != s_camera.stride ||
      frame->format != H2_PAL_CAMERA_FORMAT_UYVY)
    return H2_PAL_ERR_INVALID_STATE;
  return return_pending();
}

static h2_pal_result_t start(void* user) {
  (void)user;
  if (atomic_flag_test_and_set_explicit(&s_operation, memory_order_acquire))
    return H2_PAL_ERR_BUSY;
  h2_pal_result_t rc = start_impl();
  atomic_flag_clear_explicit(&s_operation, memory_order_release);
  return rc;
}
static h2_pal_result_t stop(void* user) {
  (void)user;
  if (atomic_flag_test_and_set_explicit(&s_operation, memory_order_acquire))
    return H2_PAL_ERR_BUSY;
  h2_pal_result_t rc = stop_impl();
  atomic_flag_clear_explicit(&s_operation, memory_order_release);
  return rc;
}
static h2_pal_result_t acquire(void* user, h2_pal_camera_frame_t* out) {
  (void)user;
  if (atomic_flag_test_and_set_explicit(&s_operation, memory_order_acquire))
    return H2_PAL_ERR_BUSY;
  h2_pal_result_t rc = acquire_impl(out);
  atomic_flag_clear_explicit(&s_operation, memory_order_release);
  return rc;
}
static h2_pal_result_t release(void* user, const h2_pal_camera_frame_t* frame) {
  (void)user;
  if (atomic_flag_test_and_set_explicit(&s_operation, memory_order_acquire))
    return H2_PAL_ERR_BUSY;
  h2_pal_result_t rc = release_impl(frame);
  atomic_flag_clear_explicit(&s_operation, memory_order_release);
  return rc;
}
const h2_pal_camera_api_t* h2_esp_board_camera_api(void) {
  static const h2_pal_camera_vtable_t ops = {
      .start = start, .acquire = acquire, .release = release, .stop = stop};
  static const h2_pal_camera_api_t api = {.vtable = &ops};
  return &api;
}
