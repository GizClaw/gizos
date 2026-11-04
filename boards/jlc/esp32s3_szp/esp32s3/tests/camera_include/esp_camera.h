#ifndef H2_SZP_TEST_CAMERA_H
#define H2_SZP_TEST_CAMERA_H
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
enum {
  ESP_OK,
  ESP_ERR_NO_MEM,
  ESP_ERR_TIMEOUT,
  ESP_ERR_INVALID_ARG,
  ESP_ERR_INVALID_STATE,
  ESP_ERR_NOT_FOUND,
  ESP_ERR_CAMERA_NOT_DETECTED,
  ESP_ERR_NOT_SUPPORTED,
  ESP_ERR_CAMERA_NOT_SUPPORTED,
  ESP_FAIL,
};
enum {
  LEDC_TIMER_1 = 1,
  LEDC_CHANNEL_1 = 1,
  PIXFORMAT_YUV422 = 1,
  PIXFORMAT_RGB565,
  FRAMESIZE_QVGA,
  CAMERA_FB_IN_PSRAM,
  CAMERA_GRAB_LATEST,
  GC0308_PID = 0x9b,
};
typedef struct {
  int pin_pwdn, pin_reset, pin_xclk, pin_sccb_sda, pin_sccb_scl, sccb_i2c_port;
  int pin_d0, pin_d1, pin_d2, pin_d3, pin_d4, pin_d5, pin_d6, pin_d7;
  int pin_vsync, pin_href, pin_pclk, xclk_freq_hz;
  int ledc_timer, ledc_channel, pixel_format, frame_size, jpeg_quality;
  int fb_count, fb_location, grab_mode;
} camera_config_t;
typedef struct {
  uint8_t* buf;
  size_t len, width, height;
  int format;
} camera_fb_t;
typedef struct sensor {
  struct {
    unsigned PID;
  } id;
  int (*set_pixformat)(struct sensor*, int);
  int (*set_hmirror)(struct sensor*, int);
} sensor_t;
esp_err_t esp_camera_init(const camera_config_t* config);
esp_err_t esp_camera_deinit(void);
sensor_t* esp_camera_sensor_get(void);
camera_fb_t* esp_camera_fb_get(void);
void esp_camera_fb_return(camera_fb_t* frame);
#endif
