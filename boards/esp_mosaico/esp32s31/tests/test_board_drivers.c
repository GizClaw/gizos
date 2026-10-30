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
static struct bmm150_mag_data magnetic_data = {12, 2, 3};
static int magnetic_init_error, magnetic_read_error;
#if H2_MOSAICO_TEST_DRIVER_IMU
static bsp_magnetometer_t expected_magnetometer;
#endif
static int ai_level = 1, boot_level = 1;
static uint16_t battery_percent = 50, battery_status = 1u << 3;
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
  assert(pin == BSP_BUTTON_AI_GPIO || pin == BSP_BUTTON_BOOT_GPIO);
  return pin == BSP_BUTTON_AI_GPIO ? ai_level : boot_level;
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
#if H2_MOSAICO_TEST_DRIVER_IMU
  assert(s == expected_magnetometer);
#endif
  return magnetic_init_error;
}
int bsp_magnetometer_read(bsp_magnetometer_t s, struct bmm150_mag_data* d) {
  (void)s;
#if H2_MOSAICO_TEST_DRIVER_IMU
  assert(s == expected_magnetometer);
#endif
  *d = magnetic_data;
  return magnetic_read_error;
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
    case 0x0a:
      v = battery_status;
      break;
    case 8:
      v = 3800;
      break;
    case 12:
      v = 0xfff6;
      break;
    case 0x2c:
      v = battery_percent;
      break;
    default:
      assert(0);
      v = 0;
  }
  out[0] = (uint8_t)(v & 0xffu);
  out[1] = (uint8_t)(v >> 8);
  return battery_error;
}
#if H2_MOSAICO_TEST_DRIVER_BUTTON
static int count_device(void* user, const h2_pal_periph_info_t* info) {
  assert(info && info->id && info->name[0]);
  ++*(unsigned*)user;
  return H2_PAL_OK;
}
#endif
#if H2_MOSAICO_TEST_DRIVER_IMU
static void check_magnetic_read(h2_pal_periph_id_t id) {
  h2_pal_imu_reading_t reading;
  memset(&reading, 0xff, sizeof(reading));
  assert(h2_pal_imu_read(h2_mosaico_imu(), id, &reading) == H2_PAL_OK);
  assert(reading.id == id && reading.flags == H2_PAL_IMU_HAS_MAG);
  assert(reading.mag_mgauss.x == (int32_t)magnetic_data.x * 10);
  assert(reading.mag_mgauss.y == (int32_t)magnetic_data.y * 10);
  assert(reading.mag_mgauss.z == (int32_t)magnetic_data.z * 10);
  assert(reading.accel_mg.x == 0 && reading.accel_mg.y == 0 &&
         reading.accel_mg.z == 0 && reading.gyro_mdps.x == 0 &&
         reading.gyro_mdps.y == 0 && reading.gyro_mdps.z == 0);
}

static void check_magnetic_failure(h2_pal_periph_id_t id, int expected) {
  h2_pal_imu_reading_t reading;
  memset(&reading, 0xff, sizeof(reading));
  assert(h2_pal_imu_read(h2_mosaico_imu(), id, &reading) == expected);
  const unsigned char* bytes = (const unsigned char*)&reading;
  for (size_t i = 0; i < sizeof(reading); ++i) assert(bytes[i] == 0);
}

static void test_magnetic_samples(void) {
  for (h2_pal_periph_id_t id = 202; id <= 203; ++id) {
    expected_magnetometer = id == 202 ? BSP_MAGNETOMETER_0 : BSP_MAGNETOMETER_1;
    magnetic_data = (struct bmm150_mag_data){12, 2, 3};
    check_magnetic_read(id);
    for (unsigned mask = 1; mask < 8; ++mask) {
      magnetic_data = (struct bmm150_mag_data){mask & 1 ? INT16_MIN : 12,
                                               mask & 2 ? INT16_MIN : -2,
                                               mask & 4 ? INT16_MIN : 3};
      check_magnetic_read(id);
    }
    /* Upstream includes this compensated output in a no-contact sample. */
    magnetic_data = (struct bmm150_mag_data){-1198, INT16_MIN, 484};
    check_magnetic_read(id);
    magnetic_data = (struct bmm150_mag_data){0, INT16_MIN + 1, INT16_MAX};
    check_magnetic_read(id);
    magnetic_data = (struct bmm150_mag_data){12, 2, 3};
    check_magnetic_read(id);

    const int errors[] = {ESP_ERR_TIMEOUT, -1};
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
      const int expected =
          errors[i] == ESP_ERR_TIMEOUT ? H2_PAL_ERR_TIMEOUT : H2_PAL_ERR_IO;
      magnetic_init_error = errors[i];
      check_magnetic_failure(id, expected);
      magnetic_init_error = ESP_OK;
      check_magnetic_read(id);
      magnetic_read_error = errors[i];
      check_magnetic_failure(id, expected);
      magnetic_read_error = ESP_OK;
      check_magnetic_read(id);
    }
  }
}
#endif
int main(void) {
  assert(h2_mosaico_peripherals_init() == H2_PAL_OK);
#if H2_MOSAICO_TEST_DRIVER_BUTTON
  unsigned count = 0;
  assert(h2_pal_periph_list(h2_mosaico_periph(), H2_PAL_PERIPH_TYPE_ANY,
                            count_device, &count) == 0 &&
         count == 7);
  count = 0;
  assert(h2_pal_periph_list(h2_mosaico_periph(), H2_PAL_PERIPH_TYPE_IMU,
                            count_device, &count) == 0 &&
         count == 3);
  h2_pal_periph_info_t info;
  assert(h2_pal_periph_get(h2_mosaico_periph(), 301, &info) == 0 &&
         info.type == H2_PAL_PERIPH_TYPE_BATTERY);
  assert(h2_pal_periph_get(h2_mosaico_periph(), 999, &info) ==
         H2_PAL_ERR_NOT_FOUND);
  h2_pal_single_button_reading_t button;
  assert(h2_pal_button_read_single_button(h2_mosaico_button(), 101, &button) ==
             0 &&
         button.state == H2_PAL_BUTTON_STATE_RELEASED);
  ai_level = BSP_BUTTON_ACTIVE_LEVEL;
  assert(h2_pal_button_read_single_button(h2_mosaico_button(), 101, &button) ==
             0 &&
         button.state == H2_PAL_BUTTON_STATE_PRESSED);
  boot_level = BSP_BUTTON_ACTIVE_LEVEL;
  assert(h2_pal_button_read_single_button(h2_mosaico_button(), 102, &button) ==
             0 &&
         button.state == H2_PAL_BUTTON_STATE_PRESSED);
  assert(h2_pal_button_read_single_button(h2_mosaico_button(), 999, &button) ==
         H2_PAL_ERR_NOT_FOUND);
#elif H2_MOSAICO_TEST_DRIVER_IMU
  h2_pal_imu_reading_t imu;
  assert(h2_pal_imu_read(h2_mosaico_imu(), 201, &imu) == 0 &&
         imu.flags == (H2_PAL_IMU_HAS_ACCEL | H2_PAL_IMU_HAS_GYRO) &&
         imu.accel_mg.x == 1000 && imu.accel_mg.z == -1000);
  test_magnetic_samples();
#elif H2_MOSAICO_TEST_DRIVER_BATTERY
  h2_pal_battery_reading_t battery;
  assert(h2_pal_input_read_battery(h2_mosaico_input(), 301, &battery) == 0 &&
         battery.voltage_mv == 3800 && battery.current_ma == -10 &&
         battery.percent_x100 == 5000 &&
         (battery.flags & H2_PAL_BATTERY_PRESENT));
  battery_status = 0;
  assert(h2_pal_input_read_battery(h2_mosaico_input(), 301, &battery) == 0 &&
         !(battery.flags & H2_PAL_BATTERY_PRESENT));
  battery_status = 1u << 3;
  assert(h2_pal_input_read_battery(h2_mosaico_input(), 301, &battery) == 0 &&
         battery_adds == 1);
  battery_percent = 101;
  assert(h2_pal_input_read_battery(h2_mosaico_input(), 301, &battery) ==
             H2_PAL_ERR_IO &&
         battery.flags == 0);
  battery_percent = 50;
  battery_error = ESP_ERR_TIMEOUT;
  assert(h2_pal_input_read_battery(h2_mosaico_input(), 301, &battery) ==
             H2_PAL_ERR_TIMEOUT &&
         battery.flags == 0);
  battery_error = 0;
#elif H2_MOSAICO_TEST_DRIVER_LIFECYCLE
  assert(h2_mosaico_peripherals_deinit() == 0);
  gpio_error = ESP_ERR_TIMEOUT;
  assert(h2_mosaico_peripherals_init() == H2_PAL_ERR_TIMEOUT);
  gpio_error = 0;
  assert(h2_mosaico_peripherals_init() == 0);
  const h2_pal_pwm_switch_api_t* motor = h2_mosaico_motor();
  assert(h2_pal_pwm_switch_set_duty(motor, 401, 10100) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_pal_pwm_switch_set_duty(motor, 401, 1550) == 0);
  uint16_t duty = 0;
  assert(h2_pal_pwm_switch_get_duty(motor, 401, &duty) == 0 && duty == 1500);
  h2_pal_battery_reading_t battery;
  assert(h2_pal_input_read_battery(h2_mosaico_input(), 301, &battery) == 0);
  motor_error = ESP_ERR_TIMEOUT;
  remove_error = ESP_ERR_TIMEOUT;
  assert(h2_mosaico_peripherals_deinit() == H2_PAL_ERR_TIMEOUT &&
         battery_removes == 1);
  motor_error = remove_error = 0;
  assert(h2_mosaico_peripherals_deinit() == 0 && battery_removes == 2);
  unsigned stops = motor_stops;
  assert(h2_mosaico_peripherals_deinit() == 0 && motor_stops == stops);
  assert(h2_mosaico_peripherals_init() == 0);
  assert(h2_pal_input_read_battery(h2_mosaico_input(), 301, &battery) == 0 &&
         battery_adds == 2);
  assert(h2_mosaico_peripherals_deinit() == 0 && motor_stops == stops);
#else
#error Select a board driver test group
#endif
  assert(h2_mosaico_peripherals_deinit() == H2_PAL_OK);
  assert(!held);
  return 0;
}
