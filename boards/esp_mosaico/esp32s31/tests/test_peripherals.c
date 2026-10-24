#ifdef NDEBUG
#undef NDEBUG
#endif
#include "h2_mosaico_peripherals.h"
#include "bsp/esp_mosaico.h"
#include "freertos/semphr.h"
#include <assert.h>
#include <string.h>
static int gpio_error, motor_error, battery_error, remove_error, held;
static unsigned motor_stops, battery_adds, battery_removes;
static int16_t magnetic_x = 12;
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* p) {
  return p;
}
int xSemaphoreTake(SemaphoreHandle_t p, unsigned timeout) {
  (void)timeout;
  assert(p && !held);
  held = 1;
  return 1;
}
void xSemaphoreGive(SemaphoreHandle_t p) {
  assert(p && held);
  held = 0;
}
int gpio_config(const gpio_config_t* p) {
  assert(p);
  return gpio_error;
}
int gpio_get_level(int pin) {
  (void)pin;
  return 1;
}
int bsp_imu_init(void) { return 0; }
int bsp_imu_start(const void* p) {
  assert(!p);
  return 0;
}
int bsp_imu_stop(void) { return 0; }
int bsp_imu_deinit(void) { return 0; }
int bsp_imu_get_accel(float* x, float* y, float* z) {
  *x = 1;
  *y = 0;
  *z = -1;
  return 0;
}
int bsp_imu_get_gyro(float* x, float* y, float* z) {
  *x = 0;
  *y = 0;
  *z = 0;
  return 0;
}
int bsp_magnetometer_init(bsp_magnetometer_t s) {
  assert(s == 0 || s == 1);
  return 0;
}
int bsp_magnetometer_read(bsp_magnetometer_t s, struct bmm150_mag_data* d) {
  (void)s;
  *d = (struct bmm150_mag_data){magnetic_x, 2, 3};
  return 0;
}
int bsp_magnetometer_deinit(bsp_magnetometer_t s) {
  (void)s;
  return 0;
}
int bsp_motor_init(void) { return 0; }
int bsp_motor_set_strength(uint8_t percent) {
  assert(percent <= 100);
  return 0;
}
int bsp_motor_set(bool on) {
  assert(!on);
  ++motor_stops;
  return motor_error;
}
void* bsp_i2c_get_handle(void) { return &battery_adds; }
int i2c_master_bus_add_device(void* b, const i2c_device_config_t* c,
                              i2c_master_dev_handle_t* p) {
  assert(b && c->device_address == 0x55);
  *p = &battery_adds;
  ++battery_adds;
  return 0;
}
int i2c_master_bus_rm_device(i2c_master_dev_handle_t d) {
  assert(d);
  ++battery_removes;
  return remove_error;
}
int i2c_master_transmit_receive(i2c_master_dev_handle_t d, const uint8_t* c,
                                size_t n, uint8_t* out, size_t size,
                                unsigned timeout) {
  (void)timeout;
  assert(d && n == 1 && size == 2);
  uint16_t v;
  switch (*c) {
    case 8:
      v = 3800;
      break;
    case 12:
      v = 0xfff6;
      break;
    case 0x2c:
      v = 50;
      break;
    default:
      assert(0);
      v = 0;
  }
  out[0] = (uint8_t)(v & 0xffu);
  out[1] = (uint8_t)(v >> 8);
  return battery_error;
}
int main(void) {
  gpio_error = ESP_ERR_TIMEOUT;
  assert(h2_mosaico_peripherals_init() == H2_PAL_ERR_TIMEOUT);
  gpio_error = 0;
  assert(h2_mosaico_peripherals_init() == 0);
  const h2_pal_pwm_switch_api_t* m = h2_mosaico_motor();
  assert(h2_pal_pwm_switch_set_duty(m, 401, 10100) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_pal_pwm_switch_set_duty(m, 401, 1550) == 0);
  uint16_t duty = 0;
  assert(h2_pal_pwm_switch_get_duty(m, 401, &duty) == 0 && duty == 1500);
  h2_pal_imu_reading_t imu;
  assert(h2_pal_imu_read(h2_mosaico_imu(), 202, &imu) == 0 &&
         imu.mag_mgauss.x == 120);
  magnetic_x = INT16_MIN;
  memset(&imu, 0xff, sizeof(imu));
  assert(h2_pal_imu_read(h2_mosaico_imu(), 203, &imu) == H2_PAL_ERR_IO &&
         imu.flags == 0);
  h2_pal_battery_reading_t battery;
  assert(h2_pal_input_read_battery(h2_mosaico_input(), 301, &battery) == 0 &&
         battery.current_ma == -10 && battery.percent_x100 == 5000);
  motor_error = ESP_ERR_TIMEOUT;
  remove_error = ESP_ERR_TIMEOUT;
  assert(h2_mosaico_peripherals_deinit() == H2_PAL_ERR_TIMEOUT &&
         battery_removes == 1);
  motor_error = 0;
  remove_error = 0;
  assert(h2_mosaico_peripherals_deinit() == 0 && battery_removes == 2);
  unsigned stops = motor_stops;
  assert(h2_mosaico_peripherals_deinit() == 0 && motor_stops == stops);
  assert(h2_mosaico_peripherals_init() == 0);
  assert(h2_pal_input_read_battery(h2_mosaico_input(), 301, &battery) == 0 &&
         battery_adds == 2);
  assert(h2_mosaico_peripherals_deinit() == 0 && motor_stops == stops);
  assert(!held);
  return 0;
}
