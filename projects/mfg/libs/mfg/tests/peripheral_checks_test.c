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
  int motor_error, motor_read_error;
  uint16_t duty;
  uint32_t battery_flags;
  int32_t current_ma, voltage_mv;
  uint16_t percent_x100;
  int32_t mag_value[2], mag_y[2], mag_z[2];
  int mag_error[2], mag_missing[2];
  unsigned mag_reads[2];
  unsigned camera_releases, camera_stops, camera_starts, failures_remaining;
  h2_pal_semaphore_t* start_entered;
  h2_pal_semaphore_t* start_resume;
} fake_t;

static int start(void* user) {
  fake_t* f = user;
  assert(!f->started);
  f->started = 1;
  ++f->camera_starts;
  if (f->start_entered) {
    assert(h2_pal_semaphore_give(f->rt.sync, f->start_entered) == 0);
    assert(h2_pal_semaphore_take(f->rt.sync, f->start_resume,
                                 H2_PAL_SYNC_WAIT_FOREVER) == 0);
  }
  if (f->failures_remaining) {
    --f->failures_remaining;
    return H2_PAL_ERR_IO;
  }
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
  unsigned i = id - 202;
  ++f->mag_reads[i];
  if (f->mag_error[i]) return f->mag_error[i];
  *out = (h2_pal_imu_reading_t){
      .id = id,
      .flags = f->mag_missing[i] ? 0 : H2_PAL_IMU_HAS_MAG,
      .mag_mgauss = {.x = f->mag_value[i], .y = f->mag_y[i], .z = f->mag_z[i]}};
  return 0;
}
static int battery(void* user, h2_pal_periph_id_t id,
                   h2_pal_battery_reading_t* out) {
  fake_t* f = user;
  assert(id == 301);
  *out = (h2_pal_battery_reading_t){.id = id,
                                    .flags = f->battery_flags,
                                    .voltage_mv = f->voltage_mv,
                                    .current_ma = f->current_ma,
                                    .percent_x100 = f->percent_x100};
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
  fake_t* f = user;
  if (f->motor_read_error) return H2_PAL_ERR_IO;
  *duty = f->duty;
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
  f->voltage_mv = 3800;
  f->percent_x100 = 6500;
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
  for (unsigned i = 1; i <= 3; ++i) assert(tick(&f, i * 200) == 0);
  assert(f.m.model.ready);
  f.release_error = 1;
  assert(tick(&f, 800) == H2_PAL_ERR_BUSY && !f.m.model.ready);
  unsigned releases = f.camera_releases;
  assert(tick(&f, 1000) == H2_PAL_ERR_INVALID_STATE);
  assert(f.camera_releases == releases && f.m.camera_borrowed);
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

static void live_magnet_axes_recover_independently(void) {
  fake_t f;
  setup(&f, H2_MFG_MAGNETOMETERS);
  f.profile.live_magnets = 1;
  f.profile.magnetic_invalid_mgauss = -327680;
  f.profile.magnetic_delta_mgauss = 80;
  f.mag_value[0] = -327680;
  assert(h2_mfg_hardware_enter(&f.m, 0) == 0);
  assert(tick(&f, 0) == 0 && !f.m.model.ready);
  f.mag_value[0] = 1000;
  assert(tick(&f, 20) == 0 && !f.m.magnet_present[0]);
  f.mag_value[0] = 1079;
  assert(tick(&f, 40) == 0 && !f.m.magnet_present[0]);
  f.mag_y[0] = 80;
  assert(tick(&f, 60) == 0 && f.m.magnet_present[0] && !f.m.model.ready);
  f.mag_value[1] = -80;
  assert(tick(&f, 80) == 0 && f.m.model.ready);
  f.mag_y[0] = f.mag_value[1] = 0;
  assert(tick(&f, 100) == 0 && f.m.model.ready);
  assert(!f.m.magnet_present[0] && !f.m.magnet_present[1]);
  f.mag_error[0] = H2_PAL_ERR_IO;
  unsigned reads = f.mag_reads[1];
  f.mag_value[1] = 80;
  assert(tick(&f, 120) == H2_PAL_ERR_IO && !f.m.model.ready);
  assert(f.mag_reads[1] == reads + 1 && f.m.magnet_present[1]);
  assert(f.m.magnet_error[0] == H2_PAL_ERR_IO && !f.m.magnet_error[1]);
  f.mag_error[0] = 0;
  f.mag_value[0] = INT32_MIN;
  assert(tick(&f, 140) == 0 && !f.m.magnet_present[0]);
  f.mag_value[0] = INT32_MAX;
  assert(tick(&f, 160) == 0 && f.m.magnet_present[0]);
  f.mag_value[0] = f.mag_y[0] = f.mag_z[0] = -327680;
  assert(tick(&f, 180) == 0 && !f.m.magnet_present[0] && !f.m.model.ready);
  f.mag_value[0] = f.mag_y[0] = f.mag_z[0] = 0;
  assert(tick(&f, 200) == 0 && !f.m.magnet_present[0]);
  assert(f.m.magnet_responded[0]);
  f.mag_missing[1] = 1;
  assert(tick(&f, 220) == H2_PAL_ERR_FORMAT && !f.m.model.ready);
  assert(h2_mfg_hardware_leave(&f.m) == 0);
}

static void manual_battery_requires_each_stage_reading(void) {
  fake_t f;
  setup(&f, H2_MFG_BATTERY);
  f.profile.battery_manual_charge = 1;
  f.battery_flags = H2_PAL_BATTERY_HAS_VOLTAGE_MV |
                    H2_PAL_BATTERY_HAS_PERCENT_X100 | H2_PAL_BATTERY_PRESENT;
  assert(h2_mfg_hardware_enter(&f.m, 0) == 0);
  assert(tick(&f, 0) == H2_PAL_ERR_IO && !f.m.model.ready);
  f.battery_flags |= H2_PAL_BATTERY_HAS_CURRENT_MA | H2_PAL_BATTERY_FULL;
  f.current_ma = -123; /* A full cell need not have positive current. */
  for (unsigned i = 1; i <= 3; ++i) assert(tick(&f, i * 20) == 0);
  assert(f.m.model.ready && f.m.battery_phase == 0);
  assert(strstr(f.m.detail, "ma=-123") && strstr(f.m.detail, "full=1"));
  f.m.battery_phase = 1;
  f.m.model.ready = 0; /* Parent consumes the phase0 operator confirmation. */
  f.battery_flags &= ~H2_PAL_BATTERY_PRESENT;
  assert(tick(&f, 80) == H2_PAL_ERR_IO && !f.m.model.ready);
  f.battery_flags |= H2_PAL_BATTERY_PRESENT;
  assert(tick(&f, 100) == 0 && f.m.model.ready && f.m.phase == 1);
  assert(strstr(f.m.detail, "unplug_confirm"));
  f.percent_x100 = 10001;
  assert(tick(&f, 120) == H2_PAL_ERR_IO && !f.m.model.ready);
  assert(h2_mfg_hardware_leave(&f.m) == 0);
}

static void held_vibration_requires_drive_then_release(void) {
  fake_t f;
  setup(&f, H2_MFG_VIBRATION);
  f.profile.hold_vibration = 1;
  f.profile.vibration_percent = 50;
  assert(h2_mfg_hardware_enter(&f.m, 0) == 0);
  assert(tick(&f, 0) == 0 && !f.duty && !f.m.model.ready);
  f.m.vibration_requested = 1;
  f.motor_read_error = 1;
  assert(tick(&f, 20) == H2_PAL_ERR_IO && !f.m.vibration_observed);
  f.motor_read_error = 0;
  assert(tick(&f, 40) == 0 && f.duty == 5000 && !f.m.model.ready);
  f.m.vibration_requested = 0;
  f.motor_error = 1;
  assert(tick(&f, 60) == H2_PAL_ERR_BUSY && !f.m.model.ready);
  f.motor_error = 0;
  assert(tick(&f, 80) == 0 && f.duty == 0 && f.m.model.ready);
  f.m.vibration_requested = 1;
  assert(tick(&f, 100) == 0 && !f.m.model.ready);
  assert(h2_mfg_hardware_leave(&f.m) == 0 && !f.duty);
}

static void sized_camera_retries_cleanly(void) {
  fake_t f;
  setup(&f, H2_MFG_CAMERA);
  f.profile.preview_width = 384;
  f.profile.preview_height = 216;
  f.failures_remaining = 2;
  assert(h2_mfg_hardware_enter(&f.m, 0) == 0);
  assert(f.camera_starts == 3 && f.camera_stops == 2);
  assert(f.m.preview_width == 384 && f.m.preview_height == 216);
  assert(tick(&f, 200) == 0);
  assert(strstr(f.m.detail, "preview=384x192"));
  assert(h2_mfg_hardware_leave(&f.m) == 0 && !f.started);

  setup(&f, H2_MFG_CAMERA);
  f.profile.preview_width = 384;
  f.profile.preview_height = 216;
  f.start_error = 1;
  assert(h2_mfg_hardware_enter(&f.m, 0) == H2_PAL_ERR_IO);
  assert(f.camera_starts == 3 && f.camera_stops == 2 && f.m.camera_started);
  assert(h2_mfg_hardware_leave(&f.m) == 0);

  setup(&f, H2_MFG_CAMERA);
  f.profile.preview_width = 384;
  f.profile.preview_height = 216;
  f.start_error = f.stop_error = 1;
  assert(h2_mfg_hardware_enter(&f.m, 0) == H2_PAL_ERR_BUSY);
  assert(f.camera_starts == 1 && f.m.camera_started && f.m.camera_preview);
  f.stop_error = 0;
  assert(h2_mfg_hardware_leave(&f.m) == 0);
}

static void factory_camera_worker_keeps_control_available(void) {
  fake_t f;
  setup(&f, H2_MFG_CAMERA);
  f.profile.camera_async = 1;
  f.profile.preview_width = 384;
  f.profile.preview_height = 216;
  f.rt.sync = h2_desktop_platform_sync_api();
  f.rt.time = h2_desktop_platform_time_api();
  f.rt.task = h2_desktop_platform_task_api();
  h2_pal_semaphore_config_t config = {
      .name = "camera_test", .allocator = f.rt.mem, .max_count = 1};
  assert(h2_pal_semaphore_create(f.rt.sync, &config, &f.start_entered) == 0);
  assert(h2_pal_semaphore_create(f.rt.sync, &config, &f.start_resume) == 0);
  assert(h2_mfg_hardware_enter(&f.m, 0) == 0 && f.m.camera_worker);
  assert(h2_pal_semaphore_take(f.rt.sync, f.start_entered, 2000) == 0);
  /* Provider start is blocked on the worker; controller tick must return. */
  assert(tick(&f, 200) == 0 && !f.m.model.ready);
  assert(h2_pal_semaphore_give(f.rt.sync, f.start_resume) == 0);
  for (unsigned i = 0; i < 200 && !f.m.model.ready; ++i) {
    assert(h2_pal_time_sleep_ms(f.rt.time, 10) == 0);
    assert(tick(&f, 400 + i * 20) == 0);
  }
  assert(f.m.model.ready && f.m.samples >= 3 && f.m.camera_preview);
  /* Tell the provider to retain its handle. Join establishes test visibility.
   */
  f.stop_error = 1;
  assert(h2_mfg_hardware_leave(&f.m) == H2_PAL_ERR_BUSY);
  assert(f.m.camera_worker && f.m.camera_preview && f.started);
  f.stop_error = 0;
  assert(h2_mfg_hardware_leave(&f.m) == 0);
  assert(!f.m.camera_worker && !f.m.camera_preview && !f.started && !f.loaned);
  assert(h2_pal_semaphore_destroy(f.rt.sync, f.start_entered) == 0);
  assert(h2_pal_semaphore_destroy(f.rt.sync, f.start_resume) == 0);
}

int main(void) {
  factory_camera_worker_keeps_control_available();
  live_magnet_axes_recover_independently();
  manual_battery_requires_each_stage_reading();
  held_vibration_requires_drive_then_release();
  sized_camera_retries_cleanly();
  camera_preserves_source_aspect();
  camera_preview_and_retained_loan();
  both_magnets_and_battery_presence_required();
  vibration_stops_before_handoff();
  return 0;
}
