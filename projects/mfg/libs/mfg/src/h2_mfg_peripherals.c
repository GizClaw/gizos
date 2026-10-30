#include "h2_mfg_internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

int h2_mfg_peripheral_enter(h2_mfg_t* m) {
  h2_mfg_check_t check = m->profile->steps[m->model.current].check;
  if (check == H2_MFG_MAGNETOMETERS) {
    for (unsigned i = 0; i < H2_MFG_MAGNETOMETER_MAX; ++i)
      for (unsigned axis = 0; axis < 3; ++axis) {
        m->mag_min[i][axis] = INT32_MAX;
        m->mag_max[i][axis] = INT32_MIN;
      }
  } else if (check == H2_MFG_VIBRATION) {
    m->motor_duty = 0;
    m->motor_pulses = 0;
    int rc =
        h2_pal_pwm_switch_set_duty(m->rt->pwm_switch, m->profile->motor, 0);
    if (rc == 0) m->motor_started = 1;
    return rc;
  } else if (check == H2_MFG_CAMERA) {
    m->camera_preview = h2_pal_mem_alloc(
        m->rt->mem, H2_MFG_PREVIEW_WIDTH * H2_MFG_PREVIEW_HEIGHT *
                        sizeof(*m->camera_preview));
    if (m->camera_preview == NULL) return H2_PAL_ERR_NO_MEMORY;
    memset(m->camera_preview, 0,
           H2_MFG_PREVIEW_WIDTH * H2_MFG_PREVIEW_HEIGHT *
               sizeof(*m->camera_preview));
    /* A failed SDK start can retain resources for stop() to retry. */
    m->camera_started = 1;
    int rc = h2_pal_camera_start(m->rt->camera);
    if (rc == H2_PAL_ERR_UNSUPPORTED) m->camera_started = 0;
    m->camera_last_ms = m->started_ms;
    return rc;
  }
  return 0;
}

int h2_mfg_peripheral_leave(h2_mfg_t* m) {
  if (m->motor_started) {
    int rc =
        h2_pal_pwm_switch_set_duty(m->rt->pwm_switch, m->profile->motor, 0);
    if (rc != 0) return rc;
    m->motor_started = 0;
    m->motor_duty = 0;
  }
  if (m->camera_borrowed) {
    int rc = h2_pal_camera_release(m->rt->camera, &m->camera_frame);
    if (rc != 0) return rc;
    m->camera_borrowed = 0;
    memset(&m->camera_frame, 0, sizeof(m->camera_frame));
  }
  if (m->camera_started) {
    int rc = h2_pal_camera_stop(m->rt->camera);
    if (rc != 0) return rc;
    m->camera_started = 0;
  }
  h2_pal_mem_free(m->rt->mem, m->camera_preview);
  m->camera_preview = NULL;
  return 0;
}

static int magnets(h2_mfg_t* m) {
  int moved = 1;
  for (unsigned i = 0; i < m->profile->magnetometer_count; ++i) {
    int rc = h2_pal_imu_read(m->rt->imu, m->profile->magnetometers[i],
                             &m->magnetometers[i]);
    if (rc != 0) return rc;
    if (!(m->magnetometers[i].flags & H2_PAL_IMU_HAS_MAG))
      return H2_PAL_ERR_FORMAT;
    const h2_pal_vec3_i32_t* v = &m->magnetometers[i].mag_mgauss;
    const int32_t axes[] = {v->x, v->y, v->z};
    int varied = 0;
    for (unsigned axis = 0; axis < 3; ++axis) {
      if (axes[axis] < m->mag_min[i][axis]) m->mag_min[i][axis] = axes[axis];
      if (axes[axis] > m->mag_max[i][axis]) m->mag_max[i][axis] = axes[axis];
      varied |= (int64_t)m->mag_max[i][axis] - m->mag_min[i][axis] >= 100;
    }
    moved &= varied;
  }
  m->model.ready = ++m->samples >= 8 && moved;
  (void)snprintf(m->detail, sizeof(m->detail),
                 "Rotate board magnets=%u samples=%u both_changed=%d",
                 (unsigned)m->profile->magnetometer_count, m->samples, moved);
  return 0;
}

static int battery(h2_mfg_t* m) {
  int rc =
      h2_pal_input_read_battery(m->rt->input, m->profile->battery, &m->battery);
  if (rc != 0) return rc;
  uint32_t required = H2_PAL_BATTERY_HAS_VOLTAGE_MV |
                      H2_PAL_BATTERY_HAS_PERCENT_X100 | H2_PAL_BATTERY_PRESENT;
  if ((m->battery.flags & required) != required || m->battery.voltage_mv <= 0 ||
      m->battery.percent_x100 > 10000)
    return H2_PAL_ERR_IO;
  m->model.ready = ++m->samples >= 3;
  (void)snprintf(m->detail, sizeof(m->detail),
                 "Battery present voltage_mv=%" PRId32 " percent_x100=%u",
                 m->battery.voltage_mv, (unsigned)m->battery.percent_x100);
  return 0;
}

static int vibration(h2_mfg_t* m, uint64_t now) {
  uint16_t duty = (now - m->started_ms) % 2000 < 200 ? 2000 : 0;
  if (duty != m->motor_duty) {
    int rc =
        h2_pal_pwm_switch_set_duty(m->rt->pwm_switch, m->profile->motor, duty);
    if (rc != 0) return rc;
    uint16_t actual = 0;
    rc = h2_pal_pwm_switch_get_duty(m->rt->pwm_switch, m->profile->motor,
                                    &actual);
    if (rc != 0) return rc;
    if (actual != duty) return H2_PAL_ERR_IO;
    if (m->motor_duty != 0 && duty == 0) ++m->motor_pulses;
    m->motor_duty = duty;
  }
  m->model.ready = m->motor_pulses >= 3 && duty == 0;
  (void)snprintf(m->detail, sizeof(m->detail),
                 "Feel vibration 200ms/2s duty=%u pulses=%u manual=pending",
                 (unsigned)duty, m->motor_pulses);
  return 0;
}

static unsigned color(int value) {
  return (unsigned)(value < 0 ? 0 : value > 255 ? 255 : value);
}

static int camera(h2_mfg_t* m, uint64_t now) {
  if (now - m->camera_last_ms < 200) return 0;
  m->camera_last_ms = now;
  int rc = h2_pal_camera_acquire(m->rt->camera, &m->camera_frame);
  if (rc != 0) return rc;
  m->camera_borrowed = 1;
  const h2_pal_camera_frame_t* f = &m->camera_frame;
  if (!f->data || f->format != H2_PAL_CAMERA_FORMAT_UYVY || f->width < 2 ||
      f->width > 4096 || (f->width & 1) || f->height == 0 || f->height > 4096 ||
      f->stride < f->width * 2 || f->height > SIZE_MAX / f->stride ||
      f->size < (size_t)f->stride * f->height)
    return H2_PAL_ERR_FORMAT;
  unsigned width = H2_MFG_PREVIEW_WIDTH, height = H2_MFG_PREVIEW_HEIGHT;
  if (f->width * height >= f->height * width) {
    height = f->height * width / f->width;
    if (height == 0) height = 1;
  } else {
    width = f->width * height / f->height;
    if (width == 0) width = 1;
  }
  const unsigned left = (H2_MFG_PREVIEW_WIDTH - width) / 2;
  const unsigned top = (H2_MFG_PREVIEW_HEIGHT - height) / 2;
  memset(m->camera_preview, 0,
         H2_MFG_PREVIEW_WIDTH * H2_MFG_PREVIEW_HEIGHT *
             sizeof(*m->camera_preview));
  for (unsigned y = 0; y < height; ++y) {
    const uint8_t* row = f->data + (size_t)(y * f->height / height) * f->stride;
    for (unsigned x = 0; x < width; ++x) {
      unsigned sx = x * f->width / width;
      const uint8_t* pair = row + (sx & ~1u) * 2;
      int luminance = pair[(sx & 1) ? 3 : 1] - 16;
      int u = pair[0] - 128, v = pair[2] - 128;
      unsigned r = color((298 * luminance + 409 * v + 128) / 256);
      unsigned g = color((298 * luminance - 100 * u - 208 * v + 128) / 256);
      unsigned b = color((298 * luminance + 516 * u + 128) / 256);
      m->camera_preview[(top + y) * H2_MFG_PREVIEW_WIDTH + left + x] =
          (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
  }
  (void)snprintf(m->detail, sizeof(m->detail),
                 "Camera UYVY %" PRIu32 "x%" PRIu32 " stride=%" PRIu32
                 " preview=%ux%u frames=%u manual_image=pending",
                 f->width, f->height, f->stride, width, height, m->samples + 1);
  rc = h2_pal_camera_release(m->rt->camera, &m->camera_frame);
  if (rc != 0) return rc;
  m->camera_borrowed = 0;
  memset(&m->camera_frame, 0, sizeof(m->camera_frame));
  m->model.ready = ++m->samples >= 3;
  return 0;
}

int h2_mfg_peripheral_tick(h2_mfg_t* m, uint64_t now) {
  switch (m->profile->steps[m->model.current].check) {
    case H2_MFG_MAGNETOMETERS:
      return magnets(m);
    case H2_MFG_BATTERY:
      return battery(m);
    case H2_MFG_VIBRATION:
      return vibration(m, now);
    case H2_MFG_CAMERA:
      return camera(m, now);
    default:
      return 0;
  }
}
