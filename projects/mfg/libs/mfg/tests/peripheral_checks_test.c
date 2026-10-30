#include "../src/h2_mfg_internal.h"
#include "h2_desktop_platform.h"

/* Shared camera, magnetometer, battery and vibration acceptance checks. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

typedef struct fake {
  h2_mfg_t m;
  h2_runtime_t rt;
  h2_mfg_profile_t profile;
  h2_mfg_step_t step;
  h2_pal_camera_api_t camera;
  h2_pal_camera_frame_t image;
  h2_pal_imu_api_t imu;
  h2_pal_input_api_t input;
  h2_pal_pwm_switch_api_t motor;
  int started, loaned, bad_format, start_error, release_error, stop_error;
  int motor_error;
  uint16_t duty;
  uint32_t battery_flags;
  int32_t mag_value[2];
  unsigned camera_releases, camera_stops;
} fake_t;

static int start(void* user) {
  fake_t* f = user;
  assert(!f->started);
  f->started = 1;
  return f->start_error ? H2_PAL_ERR_IO : 0;
}
static int acquire(void* user, h2_pal_camera_frame_t* frame) {
  static const uint8_t image[] = {
      128, 16,  128, 235, 128, 16,  128, 235,
      128, 235, 128, 16,  128, 235, 128, 16,
  };
  fake_t* f = user;
  assert(f->started && !f->loaned);
  f->loaned = 1;
  *frame = (h2_pal_camera_frame_t){.data = image,
                                   .size = sizeof(image),
                                   .width = 4,
                                   .height = 2,
                                   .stride = f->bad_format ? 1 : 8,
                                   .token = 1,
                                   .format = H2_PAL_CAMERA_FORMAT_UYVY};
  if (f->image.data != NULL) *frame = f->image;
  return 0;
}
static int release(void* user, const h2_pal_camera_frame_t* frame) {
  fake_t* f = user;
  assert(f->loaned && frame->token == 1);
  ++f->camera_releases;
  if (f->release_error) return H2_PAL_ERR_BUSY;
  f->loaned = 0;
  return 0;
}
static int stop(void* user) {
  fake_t* f = user;
  assert(!f->loaned);
  ++f->camera_stops;
  if (f->stop_error) return H2_PAL_ERR_BUSY;
  f->started = 0;
  return 0;
}
static int mag(void* user, h2_pal_periph_id_t id, h2_pal_imu_reading_t* out) {
  fake_t* f = user;
  assert(id == 202 || id == 203);
  *out = (h2_pal_imu_reading_t){.id = id,
                                .flags = H2_PAL_IMU_HAS_MAG,
                                .mag_mgauss = {.x = f->mag_value[id - 202]}};
  return 0;
}
static int battery(void* user, h2_pal_periph_id_t id,
                   h2_pal_battery_reading_t* out) {
  fake_t* f = user;
  assert(id == 301);
  *out = (h2_pal_battery_reading_t){.id = id,
                                    .flags = f->battery_flags,
                                    .voltage_mv = 3800,
                                    .percent_x100 = 6500};
  return 0;
}
static int set_motor(void* user, h2_pal_periph_id_t id, uint16_t duty) {
  fake_t* f = user;
  assert(id == 401);
  if (f->motor_error) return H2_PAL_ERR_BUSY;
  f->duty = duty;
  return 0;
}
static int get_motor(void* user, h2_pal_periph_id_t id, uint16_t* duty) {
  assert(id == 401);
  *duty = ((fake_t*)user)->duty;
  return 0;
}
static const h2_pal_camera_vtable_t camera_ops = {
    .start = start, .acquire = acquire, .release = release, .stop = stop};
static const h2_pal_imu_vtable_t imu_ops = {.read_imu = mag};
static const h2_pal_input_vtable_t input_ops = {.read_battery = battery};
static const h2_pal_pwm_switch_vtable_t motor_ops = {.set_duty = set_motor,
                                                     .get_duty = get_motor};

static void setup(fake_t* f, h2_mfg_check_t check) {
  memset(f, 0, sizeof(*f));
  f->camera = (h2_pal_camera_api_t){.user = f, .vtable = &camera_ops};
  f->imu = (h2_pal_imu_api_t){.user = f, .vtable = &imu_ops};
  f->input = (h2_pal_input_api_t){.user = f, .vtable = &input_ops};
  f->motor = (h2_pal_pwm_switch_api_t){.user = f, .vtable = &motor_ops};
  f->step = (h2_mfg_step_t){.name = "test", .check = check};
  f->profile = (h2_mfg_profile_t){.board = "test",
                                  .revision = 1,
                                  .steps = &f->step,
                                  .total = 1,
                                  .magnetometers = {202, 203},
                                  .magnetometer_count = 2,
                                  .battery = 301,
                                  .motor = 401};
  f->rt = (h2_runtime_t){.camera = &f->camera,
                         .imu = &f->imu,
                         .input = &f->input,
                         .pwm_switch = &f->motor,
                         .mem = h2_desktop_platform_default_allocator()};
  f->m = (h2_mfg_t){.rt = &f->rt,
                    .profile = &f->profile,
                    .model = {.profile = &f->profile, .summary = {.total = 1}}};
}
static int tick(fake_t* f, uint64_t now) {
  const char* action = NULL;
  int rc = h2_mfg_hardware_tick(&f->m, now, &action);
  assert(action == NULL && f->m.model.summary.step_status[0] == 0);
  return rc;
}

static void camera_preview_and_retained_loan(void) {
  fake_t f;
  setup(&f, H2_MFG_CAMERA);
  assert(h2_mfg_hardware_enter(&f.m, 0) == 0);
  for (unsigned i = 1; i <= 3; ++i) assert(tick(&f, i * 200) == 0);
  assert(f.m.model.ready && f.camera_releases == 3);
  assert(f.m.camera_preview[5 * H2_MFG_PREVIEW_WIDTH] == 0 &&
         f.m.camera_preview[5 * H2_MFG_PREVIEW_WIDTH + 40] == 0xffff);
  assert(h2_mfg_hardware_leave(&f.m) == 0);
  assert(!f.started && f.m.camera_preview == NULL);

  setup(&f, H2_MFG_CAMERA);
  assert(h2_mfg_hardware_enter(&f.m, 0) == 0);
  f.release_error = 1;
  assert(tick(&f, 200) == H2_PAL_ERR_BUSY);
  assert(h2_mfg_hardware_leave(&f.m) == H2_PAL_ERR_BUSY);
  assert(f.m.camera_borrowed && f.m.camera_preview && !f.m.model.ready);
  f.release_error = 0;
  f.stop_error = 1;
  assert(h2_mfg_hardware_leave(&f.m) == H2_PAL_ERR_BUSY);
  assert(!f.m.camera_borrowed && f.m.camera_started && f.m.camera_preview);
  f.stop_error = 0;
  assert(h2_mfg_hardware_leave(&f.m) == 0);

  setup(&f, H2_MFG_CAMERA);
  f.start_error = 1;
  assert(h2_mfg_hardware_enter(&f.m, 0) == H2_PAL_ERR_IO);
  assert(h2_mfg_hardware_leave(&f.m) == 0 && !f.started);
  setup(&f, H2_MFG_CAMERA);
  assert(h2_mfg_hardware_enter(&f.m, 0) == 0);
  f.bad_format = 1;
  assert(tick(&f, 200) == H2_PAL_ERR_FORMAT && !f.m.model.ready);
  assert(h2_mfg_hardware_leave(&f.m) == 0 && !f.loaned);
}

static void camera_preserves_source_aspect(void) {
  static uint8_t image[2 * 4096 * 2];
  for (size_t i = 0; i < sizeof(image); i += 4) {
    image[i] = image[i + 2] = 128;
    image[i + 1] = image[i + 3] = 235;
  }
  static const struct {
    unsigned width, height, preview_width, preview_height;
  } cases[] = {
      {8, 6, 120, 90},   /* SZP 4:3: pillarbox. */
      {16, 9, 160, 90},  /* 16:9: fills the viewport. */
      {8, 4, 160, 80},   /* Wider source: letterbox. */
      {6, 8, 67, 90},    /* Portrait source. */
      {4096, 1, 160, 1}, /* Integer rounding must keep one pixel. */
      {2, 4096, 1, 90},
  };
  fake_t f;
  setup(&f, H2_MFG_CAMERA);
  assert(h2_mfg_hardware_enter(&f.m, 0) == 0);
  for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    const unsigned width = cases[i].width, height = cases[i].height;
    f.image = (h2_pal_camera_frame_t){.data = image,
                                      .size = (size_t)width * height * 2,
                                      .width = width,
                                      .height = height,
                                      .stride = width * 2,
                                      .token = 1,
                                      .format = H2_PAL_CAMERA_FORMAT_UYVY};
    assert(tick(&f, (i + 1) * 200) == 0);
    const unsigned left = (H2_MFG_PREVIEW_WIDTH - cases[i].preview_width) / 2;
    const unsigned top = (H2_MFG_PREVIEW_HEIGHT - cases[i].preview_height) / 2;
    for (unsigned y = 0; y < H2_MFG_PREVIEW_HEIGHT; ++y)
      for (unsigned x = 0; x < H2_MFG_PREVIEW_WIDTH; ++x) {
        const int in_image = x >= left && y >= top &&
                             x < left + cases[i].preview_width &&
                             y < top + cases[i].preview_height;
        assert(f.m.camera_preview[y * H2_MFG_PREVIEW_WIDTH + x] ==
               (in_image ? 0xffff : 0));
      }
  }
  assert(h2_mfg_hardware_leave(&f.m) == 0);
}

static void both_magnets_and_battery_presence_required(void) {
  fake_t f;
  setup(&f, H2_MFG_MAGNETOMETERS);
  assert(h2_mfg_hardware_enter(&f.m, 0) == 0);
  for (unsigned i = 0; i < 10; ++i) assert(tick(&f, i * 20) == 0);
  assert(!f.m.model.ready);
  f.mag_value[0] = 200;
  assert(tick(&f, 220) == 0 && !f.m.model.ready);
  f.mag_value[1] = 200;
  assert(tick(&f, 240) == 0 && f.m.model.ready);
  assert(h2_mfg_hardware_leave(&f.m) == 0);

  setup(&f, H2_MFG_BATTERY);
  assert(h2_mfg_hardware_enter(&f.m, 0) == 0);
  f.battery_flags =
      H2_PAL_BATTERY_HAS_VOLTAGE_MV | H2_PAL_BATTERY_HAS_PERCENT_X100;
  assert(tick(&f, 20) == H2_PAL_ERR_IO && !f.m.model.ready);
  f.battery_flags |= H2_PAL_BATTERY_PRESENT;
  for (unsigned i = 0; i < 3; ++i) assert(tick(&f, 40 + i * 20) == 0);
  assert(f.m.model.ready);
  assert(h2_mfg_hardware_leave(&f.m) == 0);
}

static void vibration_stops_before_handoff(void) {
  fake_t f;
  setup(&f, H2_MFG_VIBRATION);
  assert(h2_mfg_hardware_enter(&f.m, 0) == 0);
  for (unsigned i = 0; i <= 5000; i += 20) assert(tick(&f, i) == 0);
  assert(f.m.model.ready && f.m.motor_pulses == 3 && f.duty == 0);
  assert(tick(&f, 6000) == 0 && f.duty != 0);
  f.motor_error = 1;
  assert(h2_mfg_hardware_leave(&f.m) == H2_PAL_ERR_BUSY);
  assert(f.m.motor_started && f.duty != 0);
  f.motor_error = 0;
  assert(h2_mfg_hardware_leave(&f.m) == 0 && f.duty == 0);
}

int main(void) {
  camera_preserves_source_aspect();
  camera_preview_and_retained_loan();
  both_magnets_and_battery_presence_required();
  vibration_stops_before_handoff();
  return 0;
}
