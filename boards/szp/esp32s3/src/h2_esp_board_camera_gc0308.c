#include "h2_esp_board_private.h"
#include "h2_esp_szp_board_internal.h"

#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

#define H2_SZP_CAMERA_WIDTH 320u
#define H2_SZP_CAMERA_HEIGHT 240u

static camera_fb_t* borrowed;
static uint64_t token;
static bool initialized, powered, streaming;
static atomic_flag operation = ATOMIC_FLAG_INIT;

static int camera_result(esp_err_t err) {
  switch (err) {
    case ESP_OK:
      return H2_PAL_OK;
    case ESP_ERR_NO_MEM:
      return H2_PAL_ERR_NO_MEMORY;
    case ESP_ERR_TIMEOUT:
      return H2_PAL_ERR_TIMEOUT;
    case ESP_ERR_INVALID_ARG:
      return H2_PAL_ERR_INVALID_ARG;
    case ESP_ERR_INVALID_STATE:
      return H2_PAL_ERR_INVALID_STATE;
    case ESP_ERR_NOT_FOUND:
    case ESP_ERR_CAMERA_NOT_DETECTED:
      return H2_PAL_ERR_NOT_FOUND;
    case ESP_ERR_NOT_SUPPORTED:
    case ESP_ERR_CAMERA_NOT_SUPPORTED:
      return H2_PAL_ERR_UNSUPPORTED;
    default:
      return H2_PAL_ERR_IO;
  }
}

static int stop_impl(void) {
  if (borrowed != NULL) return H2_PAL_ERR_BUSY;
  streaming = false;
  if (initialized) {
    esp_err_t err = esp_camera_deinit();
    /* esp_camera_init() also deinitializes after a probe/config failure. */
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
      return camera_result(err);
    initialized = false;
  }
  if (powered) {
    if (h2_esp_szp_board_set_camera_enabled(0) != 0) return H2_PAL_ERR_IO;
    powered = false;
  }
  return H2_PAL_OK;
}

static int start_impl(void) {
  if (streaming) return H2_PAL_OK;
  if (initialized || powered) return H2_PAL_ERR_BUSY;
  if (h2_esp_szp_board_i2c_bus() == NULL) return H2_PAL_ERR_UNAVAILABLE;
  ESP_LOGI("h2_szp_camera",
           "memory before start psram=%zu dma=%zu largest_dma=%zu",
           heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           heap_caps_get_free_size(MALLOC_CAP_DMA),
           heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
  /* Keep ownership even if the standby-pin write fails partway through. */
  powered = true;
  int rc =
      h2_esp_szp_board_set_camera_enabled(1) == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
  if (rc == H2_PAL_OK) {
    vTaskDelay(pdMS_TO_TICKS(20));
    const camera_config_t config = {
        .pin_pwdn = -1, /* Standby is controlled through the shared PCA9557. */
        .pin_reset = -1,
        .pin_xclk = 5,
        .pin_sccb_sda = -1, /* Borrow the existing bus; never own/delete it. */
        .pin_sccb_scl = 2,
        .sccb_i2c_port = 0,
        .pin_d7 = 9,
        .pin_d6 = 4,
        .pin_d5 = 6,
        .pin_d4 = 15,
        .pin_d3 = 17,
        .pin_d2 = 8,
        .pin_d1 = 18,
        .pin_d0 = 16,
        .pin_vsync = 3,
        .pin_href = 46,
        .pin_pclk = 7,
        .xclk_freq_hz = 24000000,
        .ledc_timer = LEDC_TIMER_1,
        .ledc_channel = LEDC_CHANNEL_1,
        .pixel_format = PIXFORMAT_YUV422,
        .frame_size = FRAMESIZE_QVGA,
        .jpeg_quality = 12,
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };
    esp_err_t err = esp_camera_init(&config);
    /* The SDK unwinds cam_init/probe/config failures itself; a second
     * esp_camera_deinit() would fail on its already freed cam object. */
    initialized = err == ESP_OK;
    rc = camera_result(err);
  }
  if (rc == H2_PAL_OK) {
    sensor_t* sensor = esp_camera_sensor_get();
    if (sensor == NULL || sensor->id.PID != GC0308_PID) {
      ESP_LOGE("h2_szp_camera", "expected GC0308, detected pid=0x%04x",
               sensor == NULL ? 0 : sensor->id.PID);
      rc = H2_PAL_ERR_UNSUPPORTED;
    } else if (sensor->set_pixformat(sensor, PIXFORMAT_YUV422) != 0 ||
               sensor->set_hmirror(sensor, 1) != 0) {
      rc = H2_PAL_ERR_IO;
    }
  }
  if (rc != H2_PAL_OK) {
    int cleanup = stop_impl();
    return cleanup == H2_PAL_OK ? rc : cleanup;
  }
  streaming = true;
  ESP_LOGI("h2_szp_camera",
           "GC0308 ready pid=0x%02x frame=320x240 native=YUYV pal=UYVY",
           GC0308_PID);
  ESP_LOGI("h2_szp_camera",
           "memory after start psram=%zu dma=%zu largest_dma=%zu",
           heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           heap_caps_get_free_size(MALLOC_CAP_DMA),
           heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
  return H2_PAL_OK;
}

/* GC0308 YUV422 is Y0,U,Y1,V. The PAL exports U,Y0,V,Y1. This reversible
 * in-place conversion is used only while the SDK buffer is borrowed; undo it
 * before returning the buffer, so the upstream always owns native pixels. */
static void swap_yuv_bytes(uint8_t* data, size_t bytes) {
  for (size_t i = 0; i < bytes; i += 2) {
    uint8_t first = data[i];
    data[i] = data[i + 1];
    data[i + 1] = first;
  }
}

static int acquire_impl(h2_pal_camera_frame_t* out) {
  if (!streaming) return H2_PAL_ERR_INVALID_STATE;
  if (borrowed != NULL) return H2_PAL_ERR_BUSY;
  /* esp32-camera bounds fb_get() to 4000 ms; single-consumer task only. */
  camera_fb_t* frame = esp_camera_fb_get();
  if (frame == NULL) return H2_PAL_ERR_TIMEOUT;
  const size_t bytes = H2_SZP_CAMERA_WIDTH * H2_SZP_CAMERA_HEIGHT * 2u;
  if (frame->buf == NULL || frame->format != PIXFORMAT_YUV422 ||
      frame->width != H2_SZP_CAMERA_WIDTH ||
      frame->height != H2_SZP_CAMERA_HEIGHT || frame->len != bytes ||
      !esp_ptr_external_ram(frame->buf)) {
    ESP_LOGE("h2_szp_camera",
             "bad frame width=%zu height=%zu bytes=%zu format=%d", frame->width,
             frame->height, frame->len, frame->format);
    esp_camera_fb_return(frame);
    return H2_PAL_ERR_FORMAT;
  }
  swap_yuv_bytes(frame->buf, bytes);
  if (token == 0)
    ESP_LOGI("h2_szp_camera", "frame buffer verified in PSRAM bytes=%zu",
             bytes);
  borrowed = frame;
  if (++token == 0) ++token;
  *out = (h2_pal_camera_frame_t){
      .data = frame->buf,
      .size = bytes,
      .width = H2_SZP_CAMERA_WIDTH,
      .height = H2_SZP_CAMERA_HEIGHT,
      .stride = H2_SZP_CAMERA_WIDTH * 2u,
      .token = token,
      .format = H2_PAL_CAMERA_FORMAT_UYVY,
  };
  return H2_PAL_OK;
}

static int release_impl(const h2_pal_camera_frame_t* frame) {
  if (borrowed == NULL || frame->token != token ||
      frame->data != borrowed->buf || frame->size != borrowed->len ||
      frame->width != H2_SZP_CAMERA_WIDTH ||
      frame->height != H2_SZP_CAMERA_HEIGHT ||
      frame->stride != H2_SZP_CAMERA_WIDTH * 2u ||
      frame->format != H2_PAL_CAMERA_FORMAT_UYVY)
    return H2_PAL_ERR_INVALID_STATE;
  swap_yuv_bytes(borrowed->buf, borrowed->len);
  esp_camera_fb_return(borrowed);
  borrowed = NULL;
  return H2_PAL_OK;
}

static int start(void* user) {
  (void)user;
  if (atomic_flag_test_and_set_explicit(&operation, memory_order_acquire))
    return H2_PAL_ERR_BUSY;
  int rc = start_impl();
  atomic_flag_clear_explicit(&operation, memory_order_release);
  return rc;
}

static int stop(void* user) {
  (void)user;
  if (atomic_flag_test_and_set_explicit(&operation, memory_order_acquire))
    return H2_PAL_ERR_BUSY;
  int rc = stop_impl();
  atomic_flag_clear_explicit(&operation, memory_order_release);
  return rc;
}

static int acquire(void* user, h2_pal_camera_frame_t* frame) {
  (void)user;
  if (atomic_flag_test_and_set_explicit(&operation, memory_order_acquire))
    return H2_PAL_ERR_BUSY;
  int rc = acquire_impl(frame);
  atomic_flag_clear_explicit(&operation, memory_order_release);
  return rc;
}

static int release(void* user, const h2_pal_camera_frame_t* frame) {
  (void)user;
  if (atomic_flag_test_and_set_explicit(&operation, memory_order_acquire))
    return H2_PAL_ERR_BUSY;
  int rc = release_impl(frame);
  atomic_flag_clear_explicit(&operation, memory_order_release);
  return rc;
}

const h2_pal_camera_api_t* h2_esp_board_camera_api(void) {
  static const h2_pal_camera_vtable_t vtable = {
      .start = start, .acquire = acquire, .release = release, .stop = stop};
  static const h2_pal_camera_api_t api = {.vtable = &vtable};
  return &api;
}
