#include "../src/h2_esp_board_private.h"
#include "../src/h2_esp_szp_board_internal.h"
#include "esp_camera.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

static bool bus_available = true, power, fail_power_on, fail_power_off;
static bool fail_frame, missing_sensor, check_overlap;
static bool external_frame = true;
static int init_result, deinit_result, sensor_result;
static unsigned inits, deinits, returns;
static uint8_t pixels[320 * 240 * 2];
static camera_fb_t native;
static sensor_t sensor;

bool esp_ptr_external_ram(const void* pointer) {
  assert(pointer == pixels);
  return external_frame;
}

i2c_master_bus_handle_t h2_esp_szp_board_i2c_bus(void) {
  return bus_available ? (void*)1 : NULL;
}
int h2_esp_szp_board_set_camera_enabled(int enabled) {
  if (enabled && fail_power_on) return -1;
  if (!enabled && fail_power_off) return -1;
  power = enabled != 0;
  return 0;
}
void vTaskDelay(unsigned ticks) { assert(ticks > 0); }
esp_err_t esp_camera_init(const camera_config_t* config) {
  assert(power);
  assert(config->pin_pwdn == -1 && config->pin_reset == -1);
  assert(config->pin_sccb_sda == -1 && config->sccb_i2c_port == 0);
  assert(config->pin_sccb_scl == 2 && config->pin_xclk == 5);
  assert(config->pin_pclk == 7 && config->pin_vsync == 3 &&
         config->pin_href == 46);
  const int pins[] = {config->pin_d0, config->pin_d1, config->pin_d2,
                      config->pin_d3, config->pin_d4, config->pin_d5,
                      config->pin_d6, config->pin_d7};
  const int supplier[] = {16, 18, 8, 17, 15, 6, 4, 9};
  assert(memcmp(pins, supplier, sizeof(pins)) == 0);
  assert(config->xclk_freq_hz == 24000000);
  assert(config->ledc_timer == LEDC_TIMER_1 &&
         config->ledc_channel == LEDC_CHANNEL_1);
  assert(config->pixel_format == PIXFORMAT_YUV422 &&
         config->frame_size == FRAMESIZE_QVGA);
  assert(config->fb_count == 2 && config->fb_location == CAMERA_FB_IN_PSRAM &&
         config->grab_mode == CAMERA_GRAB_LATEST);
  ++inits;
  return init_result;
}
esp_err_t esp_camera_deinit(void) {
  ++deinits;
  return deinit_result;
}
static int set_format(sensor_t* value, int format) {
  assert(value == &sensor && format == PIXFORMAT_YUV422);
  return sensor_result;
}
static int set_mirror(sensor_t* value, int mirror) {
  assert(value == &sensor && mirror == 1);
  return sensor_result;
}
sensor_t* esp_camera_sensor_get(void) {
  return missing_sensor ? NULL : &sensor;
}
camera_fb_t* esp_camera_fb_get(void) {
  if (check_overlap)
    assert(h2_pal_camera_stop(h2_esp_board_camera_api()) == H2_PAL_ERR_BUSY);
  return fail_frame ? NULL : &native;
}
void esp_camera_fb_return(camera_fb_t* frame) {
  assert(frame == &native);
  /* The adapter must restore the SDK's original YUYV bytes. */
  assert(pixels[0] == 16 && pixels[1] == 128 && pixels[2] == 235 &&
         pixels[3] == 130);
  ++returns;
}
static void native_frame(void) {
  memset(pixels, 0, sizeof(pixels));
  pixels[0] = 16;
  pixels[1] = 128;
  pixels[2] = 235;
  pixels[3] = 130;
  native = (camera_fb_t){.buf = pixels,
                         .width = 320,
                         .height = 240,
                         .len = sizeof(pixels),
                         .format = PIXFORMAT_YUV422};
}

int main(void) {
  sensor = (sensor_t){.id = {.PID = GC0308_PID},
                      .set_pixformat = set_format,
                      .set_hmirror = set_mirror};
  const h2_pal_camera_api_t* api = h2_esp_board_camera_api();
  h2_pal_camera_frame_t frame, other, old;
  assert(h2_pal_camera_stop(api) == 0);
  assert(h2_pal_camera_acquire(api, &frame) == H2_PAL_ERR_INVALID_STATE);
  assert(frame.data == NULL && frame.token == 0);
  bus_available = false;
  assert(h2_pal_camera_start(api) == H2_PAL_ERR_UNAVAILABLE);
  assert(inits == 0);
  bus_available = true;
  fail_power_on = true;
  assert(h2_pal_camera_start(api) == H2_PAL_ERR_IO && !power);
  fail_power_on = false;
  init_result = ESP_ERR_CAMERA_NOT_DETECTED;
  /* Failed SDK initialization already deinitializes its driver. Calling
   * deinit again returns ESP_FAIL and must not strand the PAL in cleanup. */
  deinit_result = ESP_FAIL;
  unsigned prior_deinits = deinits;
  assert(h2_pal_camera_start(api) == H2_PAL_ERR_NOT_FOUND);
  assert(!power && deinits == prior_deinits);
  assert(h2_pal_camera_stop(api) == 0);
  deinit_result = ESP_OK;
  init_result = ESP_OK;
  sensor.id.PID = 0x1234;
  assert(h2_pal_camera_start(api) == H2_PAL_ERR_UNSUPPORTED && !power);
  sensor.id.PID = GC0308_PID;
  missing_sensor = true;
  assert(h2_pal_camera_start(api) == H2_PAL_ERR_UNSUPPORTED && !power);
  missing_sensor = false;
  sensor_result = -1;
  assert(h2_pal_camera_start(api) == H2_PAL_ERR_IO && !power);
  sensor_result = 0;
  assert(h2_pal_camera_start(api) == 0 && power);
  unsigned count = inits;
  assert(h2_pal_camera_start(api) == 0 && inits == count);
  native_frame();
  fail_frame = true;
  assert(h2_pal_camera_acquire(api, &frame) == H2_PAL_ERR_TIMEOUT);
  assert(frame.data == NULL && frame.token == 0);
  fail_frame = false;
  native.len--;
  assert(h2_pal_camera_acquire(api, &frame) == H2_PAL_ERR_FORMAT);
  assert(frame.data == NULL && returns == 1);
  native_frame();
  native.format = PIXFORMAT_RGB565;
  assert(h2_pal_camera_acquire(api, &frame) == H2_PAL_ERR_FORMAT);
  native_frame();
  native.width = 160;
  assert(h2_pal_camera_acquire(api, &frame) == H2_PAL_ERR_FORMAT);
  native_frame();
  external_frame = false;
  assert(h2_pal_camera_acquire(api, &frame) == H2_PAL_ERR_FORMAT);
  external_frame = true;
  check_overlap = true;
  assert(h2_pal_camera_acquire(api, &frame) == 0);
  assert(frame.width == 320 && frame.height == 240 && frame.stride == 640);
  assert(frame.size == sizeof(pixels) &&
         frame.format == H2_PAL_CAMERA_FORMAT_UYVY);
  assert(frame.data[0] == 128 && frame.data[1] == 16 && frame.data[2] == 130 &&
         frame.data[3] == 235);
  assert(h2_pal_camera_acquire(api, &other) == H2_PAL_ERR_BUSY);
  assert(other.data == NULL);
  assert(h2_pal_camera_stop(api) == H2_PAL_ERR_BUSY);
  old = frame;
  ++old.token;
  assert(h2_pal_camera_release(api, &old) == H2_PAL_ERR_INVALID_STATE);
  old = frame;
  old.stride--;
  assert(h2_pal_camera_release(api, &old) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_pal_camera_release(api, &frame) == 0);
  assert(h2_pal_camera_release(api, &frame) == H2_PAL_ERR_INVALID_STATE);
  old = frame;
  assert(h2_pal_camera_acquire(api, &frame) == 0 && frame.token != old.token);
  assert(h2_pal_camera_release(api, &old) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_pal_camera_release(api, &frame) == 0);
  deinit_result = ESP_FAIL;
  assert(h2_pal_camera_stop(api) == H2_PAL_ERR_IO && power);
  assert(h2_pal_camera_start(api) == H2_PAL_ERR_BUSY);
  assert(h2_pal_camera_acquire(api, &frame) == H2_PAL_ERR_INVALID_STATE);
  deinit_result = ESP_OK;
  fail_power_off = true;
  assert(h2_pal_camera_stop(api) == H2_PAL_ERR_IO && power);
  count = deinits;
  fail_power_off = false;
  assert(h2_pal_camera_stop(api) == 0 && !power && deinits == count);
  assert(h2_pal_camera_stop(api) == 0 && deinits == count);
  assert(h2_pal_camera_start(api) == 0);
  native_frame();
  assert(h2_pal_camera_acquire(api, &frame) == 0);
  assert(h2_pal_camera_release(api, &frame) == 0);
  assert(h2_pal_camera_stop(api) == 0 && !power);
  return 0;
}
