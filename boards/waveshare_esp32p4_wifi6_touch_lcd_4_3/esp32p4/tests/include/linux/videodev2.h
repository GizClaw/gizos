#ifndef H2_P4_TEST_VIDEODEV2_H
#define H2_P4_TEST_VIDEODEV2_H
#include <stdint.h>
enum {
  V4L2_BUF_TYPE_VIDEO_CAPTURE = 1,
  V4L2_MEMORY_USERPTR = 2,
  V4L2_PIX_FMT_UYVY = 3,
  V4L2_QUANTIZATION_LIM_RANGE = 4,
  V4L2_YCBCR_ENC_601 = 5,
  V4L2_BUF_FLAG_ERROR = 0x40,
  VIDIOC_G_FMT = 1000,
  VIDIOC_S_FMT,
  VIDIOC_REQBUFS,
  VIDIOC_QUERYBUF,
  VIDIOC_QBUF,
  VIDIOC_DQBUF,
  VIDIOC_STREAMON,
  VIDIOC_STREAMOFF,
};
struct v4l2_format {
  uint32_t type;
  union {
    struct {
      uint32_t width, height, pixelformat, bytesperline, sizeimage;
      uint32_t quantization, ycbcr_enc;
    } pix;
  } fmt;
};
struct v4l2_requestbuffers {
  uint32_t count, type, memory;
};
struct v4l2_buffer {
  uint32_t type, memory, index, length, bytesused, flags;
  union {
    unsigned long userptr;
  } m;
};
#endif
