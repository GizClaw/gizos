#include "h2_mfg_internal.h"
#include "h2_atomic.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

struct h2_mfg_camera {
  h2_runtime_t* rt;
  h2_pal_task_t* task;
  h2_pal_mutex_t* mutex;
  h2_atomic_int_t stop, done, error;
  int started, borrowed, cleanup_only, cleanup_error;
  h2_pal_camera_frame_t frame;
  uint16_t* preview;
  unsigned width, height, samples;
  char detail[160];
};

static int camera_worker_enter(h2_mfg_t* m);
static int camera_worker_tick(h2_mfg_t* m);
static int camera_worker_leave(h2_mfg_t* m);

/* The session mutex may be held by start/stop. Callbacks take only this lock.
 */
static bool scan_result(void* user, const h2_pal_ble_scan_result_t* result) {
  h2_mfg_t* m = user;
  if (h2_pal_mutex_lock(m->rt->sync, m->scan_mutex) != 0) return true;
  if (m->scan_started && !m->scan_done) {
    if (result == NULL || (result->local_name_len && !result->local_name)) {
      m->scan_error = H2_PAL_ERR_FORMAT;
    } else {
      unsigned i = 0;
      for (; i < m->scan_count; ++i)
        if (m->scan_devices[i].address.type == result->addr.type &&
            memcmp(m->scan_devices[i].address.value, result->addr.value,
                   sizeof(result->addr.value)) == 0)
          break;
      if (i < sizeof(m->scan_devices) / sizeof(m->scan_devices[0])) {
        if (i == m->scan_count) {
          m->scan_devices[i].address = result->addr;
          ++m->scan_count;
        }
        m->scan_devices[i].rssi = result->rssi;
        if (result->local_name_len) {
          size_t n = result->local_name_len;
          if (n >= sizeof(m->scan_devices[i].name))
            n = sizeof(m->scan_devices[i].name) - 1;
          memcpy(m->scan_devices[i].name, result->local_name, n);
          m->scan_devices[i].name[n] = '\0';
        }
      }
    }
  }
  (void)h2_pal_mutex_unlock(m->rt->sync, m->scan_mutex);
  return false;
}

static int scan_stop(h2_mfg_t* m) {
  /* Never hold scan_mutex while the provider finishes its callbacks. */
  int rc = h2_pal_ble_stop_scan(m->rt->ble_host);
  int locked = h2_pal_mutex_lock(m->rt->sync, m->scan_mutex);
  if (locked != 0) return locked;
  if (rc != 0) {
    m->scan_error = rc;
  } else {
    m->scan_started = 0;
    m->scan_done = 1;
  }
  int unlocked = h2_pal_mutex_unlock(m->rt->sync, m->scan_mutex);
  return rc != 0 ? rc : unlocked;
}

static int scan_enter(h2_mfg_t* m) {
  h2_pal_mutex_config_t config = {.name = "mfg_scan", .allocator = m->rt->mem};
  int rc = h2_pal_mutex_create(m->rt->sync, &config, &m->scan_mutex);
  if (rc != 0) return rc;
  m->scan_count = 0;
  m->scan_started = m->scan_done = m->scan_error = 0;
  memset(m->scan_devices, 0, sizeof(m->scan_devices));
  rc = h2_pal_ble_start(m->rt->ble_host);
  if (rc != 0) {
    m->scan_error = rc;
    return rc;
  }
  /* Own the callback context even if a failed start needs stop cleanup. */
  m->scan_started = 1;
  h2_pal_ble_scan_params_t params = {
      .mode = H2_PAL_BLE_SCAN_MODE_ACTIVE, .interval_ms = 100, .window_ms = 50};
  rc = h2_pal_ble_start_scan(m->rt->ble_host, &params, scan_result, m);
  if (rc != 0) {
    int locked = h2_pal_mutex_lock(m->rt->sync, m->scan_mutex);
    if (locked != 0) return locked;
    m->scan_error = rc;
    (void)h2_pal_mutex_unlock(m->rt->sync, m->scan_mutex);
  }
  return rc;
}

int h2_mfg_peripheral_enter(h2_mfg_t* m) {
  h2_mfg_check_t check = m->profile->steps[m->model.current].check;
  if (check == H2_MFG_BLE && m->profile->ble_scan_only) return scan_enter(m);
  if (check == H2_MFG_BATTERY && m->profile->battery_manual_charge) {
    m->battery_phase = m->phase = m->samples = 0;
    m->model.ready = 0;
  }
  if (check == H2_MFG_MAGNETOMETERS) {
    memset(m->mag_baseline_valid, 0, sizeof(m->mag_baseline_valid));
    memset(m->magnet_present, 0, sizeof(m->magnet_present));
    memset(m->magnet_responded, 0, sizeof(m->magnet_responded));
    memset(m->magnet_error, 0, sizeof(m->magnet_error));
    for (unsigned i = 0; i < H2_MFG_MAGNETOMETER_MAX; ++i)
      for (unsigned axis = 0; axis < 3; ++axis) {
        m->mag_min[i][axis] = INT32_MAX;
        m->mag_max[i][axis] = INT32_MIN;
      }
  } else if (check == H2_MFG_VIBRATION) {
    m->motor_duty = 0;
    m->motor_pulses = 0;
    m->vibration_requested = m->vibration_observed = 0;
    int rc =
        h2_pal_pwm_switch_set_duty(m->rt->pwm_switch, m->profile->motor, 0);
    if (rc == 0) m->motor_started = 1;
    return rc;
  } else if (check == H2_MFG_CAMERA) {
    m->preview_width = m->profile->preview_width ? m->profile->preview_width
                                                 : H2_MFG_PREVIEW_WIDTH;
    m->preview_height = m->profile->preview_height ? m->profile->preview_height
                                                   : H2_MFG_PREVIEW_HEIGHT;
    if (m->preview_width > 1024 || m->preview_height > 1024)
      return H2_PAL_ERR_INVALID_ARG;
    size_t bytes = (size_t)m->preview_width * m->preview_height *
                   sizeof(*m->camera_preview);
    m->camera_preview = h2_pal_mem_alloc(m->rt->mem, bytes);
    if (m->camera_preview == NULL) return H2_PAL_ERR_NO_MEMORY;
    memset(m->camera_preview, 0, bytes);
    m->camera_last_ms = m->started_ms;
    if (m->profile->camera_async) return camera_worker_enter(m);
    /* Sized previews opt into bounded startup recovery; defaults stay one-shot.
     */
    unsigned attempts = m->profile->preview_width ? 3 : 1;
    int rc = 0;
    for (unsigned i = 0; i < attempts; ++i) {
      /* A failed SDK start can retain resources for stop() to retry. */
      m->camera_started = 1;
      rc = h2_pal_camera_start(m->rt->camera);
      if (rc == H2_PAL_ERR_UNSUPPORTED) m->camera_started = 0;
      if (rc == 0 || rc == H2_PAL_ERR_UNSUPPORTED || i + 1 == attempts) break;
      int stopped = h2_pal_camera_stop(m->rt->camera);
      if (stopped != 0) return stopped;
      m->camera_started = 0;
    }
    return rc;
  }
  return 0;
}

int h2_mfg_peripheral_leave(h2_mfg_t* m) {
  if (m->scan_started) {
    int rc = scan_stop(m);
    if (rc != 0) return rc;
  }
  if (m->scan_mutex) {
    int rc = h2_pal_mutex_destroy(m->rt->sync, m->scan_mutex);
    if (rc != 0) return rc;
    m->scan_mutex = NULL;
  }
  if (m->motor_started) {
    int rc =
        h2_pal_pwm_switch_set_duty(m->rt->pwm_switch, m->profile->motor, 0);
    if (rc != 0) return rc;
    m->motor_started = 0;
    m->motor_duty = 0;
  }
  if (m->camera_worker) {
    int rc = camera_worker_leave(m);
    if (rc != 0) return rc;
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

static int live_magnets(h2_mfg_t* m) {
  int first_error = 0, ready = m->profile->magnetometer_count != 0;
  int32_t threshold = m->profile->magnetic_delta_mgauss;
  if (threshold <= 0) threshold = 80;
  for (unsigned i = 0; i < m->profile->magnetometer_count; ++i) {
    int rc = h2_pal_imu_read(m->rt->imu, m->profile->magnetometers[i],
                             &m->magnetometers[i]);
    if (rc == 0 && !(m->magnetometers[i].flags & H2_PAL_IMU_HAS_MAG))
      rc = H2_PAL_ERR_FORMAT;
    m->magnet_error[i] = rc;
    m->magnet_present[i] = 0;
    if (rc != 0) {
      memset(m->mag_baseline_valid[i], 0, sizeof(m->mag_baseline_valid[i]));
      if (first_error == 0) first_error = rc;
      ready = 0;
      continue; /* Read the other physical side even when this one fails. */
    }
    const h2_pal_vec3_i32_t* v = &m->magnetometers[i].mag_mgauss;
    const int32_t axes[] = {v->x, v->y, v->z};
    int valid = 0;
    for (unsigned axis = 0; axis < 3; ++axis) {
      if (axes[axis] == m->profile->magnetic_invalid_mgauss) {
        m->mag_baseline_valid[i][axis] = 0;
        continue;
      }
      ++valid;
      if (!m->mag_baseline_valid[i][axis]) {
        m->mag_baseline[i][axis] = axes[axis];
        m->mag_baseline_valid[i][axis] = 1;
        continue;
      }
      int64_t delta = (int64_t)axes[axis] - m->mag_baseline[i][axis];
      if (delta >= threshold || delta <= -(int64_t)threshold)
        m->magnet_present[i] = 1;
    }
    m->magnet_responded[i] |= m->magnet_present[i];
    ready &= valid != 0 && m->magnet_responded[i];
  }
  m->model.ready = ready && first_error == 0;
  (void)snprintf(
      m->detail, sizeof(m->detail),
      "Magnet 0 live=%d seen=%d error=%d; 1 live=%d seen=%d error=%d",
      m->magnet_present[0], m->magnet_responded[0], m->magnet_error[0],
      m->magnet_present[1], m->magnet_responded[1], m->magnet_error[1]);
  return first_error;
}

static int magnets(h2_mfg_t* m) {
  if (m->profile->live_magnets) return live_magnets(m);
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
  if (m->profile->battery_manual_charge) m->model.ready = 0;
  int rc =
      h2_pal_input_read_battery(m->rt->input, m->profile->battery, &m->battery);
  if (rc != 0) return rc;
  uint32_t required = H2_PAL_BATTERY_HAS_VOLTAGE_MV |
                      H2_PAL_BATTERY_HAS_PERCENT_X100 | H2_PAL_BATTERY_PRESENT;
  if (m->profile->battery_manual_charge)
    required |= H2_PAL_BATTERY_HAS_CURRENT_MA;
  if ((m->battery.flags & required) != required || m->battery.voltage_mv <= 0 ||
      m->battery.percent_x100 > 10000)
    return H2_PAL_ERR_IO;
  if (m->profile->battery_manual_charge) {
    if (m->battery_phase > 1) return H2_PAL_ERR_INVALID_STATE;
    if (m->phase != m->battery_phase) {
      m->phase = m->battery_phase;
      m->samples = 0;
    }
    m->model.ready = ++m->samples >= (m->battery_phase ? 1u : 3u);
    (void)snprintf(m->detail, sizeof(m->detail),
                   "Battery present=%d mv=%" PRId32 " ma=%" PRId32
                   " percent_x100=%u charging=%d full=%d manual=%s",
                   !!(m->battery.flags & H2_PAL_BATTERY_PRESENT),
                   m->battery.voltage_mv, m->battery.current_ma,
                   (unsigned)m->battery.percent_x100,
                   !!(m->battery.flags & H2_PAL_BATTERY_CHARGING),
                   !!(m->battery.flags & H2_PAL_BATTERY_FULL),
                   m->battery_phase ? "unplug_confirm" : "charge_confirm");
    return 0;
  }
  m->model.ready = ++m->samples >= 3;
  (void)snprintf(m->detail, sizeof(m->detail),
                 "Battery present voltage_mv=%" PRId32 " percent_x100=%u",
                 m->battery.voltage_mv, (unsigned)m->battery.percent_x100);
  return 0;
}

static int vibration(h2_mfg_t* m, uint64_t now) {
  uint16_t duty = (now - m->started_ms) % 2000 < 200 ? 2000 : 0;
  if (m->profile->hold_vibration) {
    uint32_t percent = m->profile->vibration_percent;
    if (percent == 0 || percent > 100) return H2_PAL_ERR_INVALID_ARG;
    duty = m->vibration_requested ? (uint16_t)(percent * 100) : 0;
    m->model.ready = 0;
  }
  if (duty != m->motor_duty || m->profile->hold_vibration) {
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
  if (m->profile->hold_vibration) {
    if (duty != 0) m->vibration_observed = 1;
    m->model.ready = m->vibration_observed && m->motor_pulses != 0 && duty == 0;
    (void)snprintf(
        m->detail, sizeof(m->detail),
        "Hold to vibrate duty=%u observed=%d released=%d manual=pending",
        (unsigned)duty, m->vibration_observed, m->model.ready);
    return 0;
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

static int camera_convert(const h2_pal_camera_frame_t* f, uint16_t* preview,
                          unsigned preview_width, unsigned preview_height,
                          char* detail, size_t detail_size, unsigned samples) {
  if (!f->data || f->format != H2_PAL_CAMERA_FORMAT_UYVY || f->width < 2 ||
      f->width > 4096 || (f->width & 1) || f->height == 0 || f->height > 4096 ||
      f->stride < f->width * 2 || f->height > SIZE_MAX / f->stride ||
      f->size < (size_t)f->stride * f->height)
    return H2_PAL_ERR_FORMAT;
  unsigned width = preview_width, height = preview_height;
  if (f->width * height >= f->height * width) {
    height = f->height * width / f->width;
    if (height == 0) height = 1;
  } else {
    width = f->width * height / f->height;
    if (width == 0) width = 1;
  }
  const unsigned left = (preview_width - width) / 2;
  const unsigned top = (preview_height - height) / 2;
  memset(preview, 0, (size_t)preview_width * preview_height * sizeof(*preview));
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
      preview[(top + y) * preview_width + left + x] =
          (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
  }
  (void)snprintf(detail, detail_size,
                 "Camera UYVY %" PRIu32 "x%" PRIu32 " stride=%" PRIu32
                 " preview=%ux%u frames=%u manual_image=pending",
                 f->width, f->height, f->stride, width, height, samples + 1);
  return 0;
}

static int camera(h2_mfg_t* m, uint64_t now) {
  if (m->camera_worker) return camera_worker_tick(m);
  if (now - m->camera_last_ms < 200) return 0;
  m->model.ready = 0;
  if (m->camera_borrowed) return H2_PAL_ERR_INVALID_STATE;
  m->camera_last_ms = now;
  int rc = h2_pal_camera_acquire(m->rt->camera, &m->camera_frame);
  if (rc != 0) return rc;
  m->camera_borrowed = 1;
  rc = camera_convert(&m->camera_frame, m->camera_preview, m->preview_width,
                      m->preview_height, m->detail, sizeof(m->detail),
                      m->samples);
  if (rc != 0) return rc;
  rc = h2_pal_camera_release(m->rt->camera, &m->camera_frame);
  if (rc != 0) return rc;
  m->camera_borrowed = 0;
  memset(&m->camera_frame, 0, sizeof(m->camera_frame));
  m->model.ready = ++m->samples >= 3;
  return 0;
}

static int camera_cleanup(h2_mfg_camera_t* c) {
  if (c->borrowed) {
    int rc = h2_pal_camera_release(c->rt->camera, &c->frame);
    if (rc != 0) return rc;
    c->borrowed = 0;
    memset(&c->frame, 0, sizeof(c->frame));
  }
  if (c->started) {
    int rc = h2_pal_camera_stop(c->rt->camera);
    if (rc != 0) return rc;
    c->started = 0;
  }
  return 0;
}

static void camera_run(void* user) {
  h2_mfg_camera_t* c = user;
  int rc = 0;
  uint64_t now = 0, last_frame = 0;
  if (!c->cleanup_only) {
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
      if (h2_atomic_int_load(&c->stop, H2_ATOMIC_SEQ_CST)) break;
      c->started = 1;
      rc = h2_pal_camera_start(c->rt->camera);
      if (rc == H2_PAL_ERR_UNSUPPORTED) c->started = 0;
      if (rc == 0 || rc == H2_PAL_ERR_UNSUPPORTED) break;
      int cleanup = camera_cleanup(c);
      if (cleanup != 0) {
        rc = cleanup;
        break;
      }
    }
    if (rc == 0) rc = h2_pal_time_get_monotonic_ms(c->rt->time, &last_frame);
    while (rc == 0 && !h2_atomic_int_load(&c->stop, H2_ATOMIC_SEQ_CST)) {
      rc = h2_pal_camera_acquire(c->rt->camera, &c->frame);
      if (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK) {
        int clock = h2_pal_time_get_monotonic_ms(c->rt->time, &now);
        if (clock != 0) {
          rc = clock;
          break;
        }
        if (now - last_frame >= 3000) {
          rc = H2_PAL_ERR_TIMEOUT;
          break;
        }
        rc = h2_pal_time_sleep_ms(c->rt->time, 10);
        continue;
      }
      if (rc != 0) break;
      c->borrowed = 1;
      rc = h2_pal_mutex_lock(c->rt->sync, c->mutex);
      if (rc != 0) break;
      rc = camera_convert(&c->frame, c->preview, c->width, c->height, c->detail,
                          sizeof(c->detail), c->samples);
      if (rc == 0) rc = h2_pal_camera_release(c->rt->camera, &c->frame);
      if (rc == 0) {
        c->borrowed = 0;
        memset(&c->frame, 0, sizeof(c->frame));
        if (c->samples < UINT32_MAX) ++c->samples;
      } else {
        h2_atomic_int_store(&c->error, rc, H2_ATOMIC_SEQ_CST);
      }
      (void)h2_pal_mutex_unlock(c->rt->sync, c->mutex);
      if (rc != 0) break;
      rc = h2_pal_time_get_monotonic_ms(c->rt->time, &last_frame);
      if (rc == 0) rc = h2_pal_time_sleep_ms(c->rt->time, 200);
    }
  }
  /* Publish failure before potentially slow cleanup so stale readiness clears.
   */
  if (rc != 0) h2_atomic_int_store(&c->error, rc, H2_ATOMIC_SEQ_CST);
  /* Only the worker calls providers, including failed-stop retry workers. */
  c->cleanup_error = camera_cleanup(c);
  if (rc == 0) rc = c->cleanup_error;
  h2_atomic_int_store(&c->error, rc, H2_ATOMIC_SEQ_CST);
  h2_atomic_int_store(&c->done, 1, H2_ATOMIC_SEQ_CST);
}

static void camera_worker_free(h2_mfg_t* m) {
  h2_mfg_camera_t* c = m->camera_worker;
  if (c->stop.storage) h2_atomic_int_destroy(&c->stop);
  if (c->done.storage) h2_atomic_int_destroy(&c->done);
  if (c->error.storage) h2_atomic_int_destroy(&c->error);
  h2_pal_mem_free(m->rt->mem, c->preview);
  h2_pal_mem_free(m->rt->mem, c);
  m->camera_worker = NULL;
}

static int camera_worker_enter(h2_mfg_t* m) {
  h2_mfg_camera_t* c = h2_pal_mem_alloc(m->rt->mem, sizeof(*c));
  if (!c) return H2_PAL_ERR_NO_MEMORY;
  memset(c, 0, sizeof(*c));
  m->camera_worker = c;
  c->rt = m->rt;
  c->width = m->preview_width;
  c->height = m->preview_height;
  h2_atomic_result_t init = h2_atomic_int_init(&c->stop, 0);
  if (init == H2_ATOMIC_OK) init = h2_atomic_int_init(&c->done, 1);
  if (init == H2_ATOMIC_OK) init = h2_atomic_int_init(&c->error, 0);
  if (init != H2_ATOMIC_OK) {
    camera_worker_free(m);
    return H2_PAL_ERR_NO_MEMORY;
  }
  c->preview = h2_pal_mem_alloc(
      m->rt->mem, (size_t)c->width * c->height * sizeof(*c->preview));
  if (!c->preview) {
    camera_worker_free(m);
    return H2_PAL_ERR_NO_MEMORY;
  }
  h2_pal_mutex_config_t config = {.name = "mfg_camera",
                                  .allocator = m->rt->mem};
  int rc = h2_pal_mutex_create(m->rt->sync, &config, &c->mutex);
  if (rc != 0) {
    camera_worker_free(m);
    return rc;
  }
  h2_pal_task_options_t options = {.name = "mfg/camera",
                                   .min_stack_size = 8192};
  h2_atomic_int_store(&c->done, 0, H2_ATOMIC_SEQ_CST);
  rc = h2_pal_task_start(m->rt->task, &options, camera_run, c, &c->task);
  if (rc != 0) {
    h2_atomic_int_store(&c->done, 1, H2_ATOMIC_SEQ_CST);
    return rc; /* Leave owns mutex/buffers even when task startup fails. */
  }
  return 0;
}

static int camera_worker_tick(h2_mfg_t* m) {
  h2_mfg_camera_t* c = m->camera_worker;
  int rc = h2_atomic_int_load(&c->error, H2_ATOMIC_SEQ_CST);
  m->model.ready = 0;
  if (rc != 0) return rc;
  if (h2_atomic_int_load(&c->stop, H2_ATOMIC_SEQ_CST)) return 0;
  rc = h2_pal_mutex_try_lock(c->rt->sync, c->mutex);
  if (rc == H2_PAL_ERR_WOULD_BLOCK || rc == H2_PAL_ERR_BUSY) return 0;
  if (rc != 0) return rc;
  if (c->samples) {
    memcpy(m->camera_preview, c->preview,
           (size_t)c->width * c->height * sizeof(*c->preview));
    memcpy(m->detail, c->detail, sizeof(m->detail));
    m->samples = c->samples;
    m->model.ready = c->samples >= 3;
  }
  (void)h2_pal_mutex_unlock(c->rt->sync, c->mutex);
  rc = h2_atomic_int_load(&c->error, H2_ATOMIC_SEQ_CST);
  if (rc != 0) m->model.ready = 0;
  return rc;
}

static int camera_worker_leave(h2_mfg_t* m) {
  h2_mfg_camera_t* c = m->camera_worker;
  h2_atomic_int_store(&c->stop, 1, H2_ATOMIC_SEQ_CST);
  m->model.ready = 0;
  for (unsigned waited = 0; !h2_atomic_int_load(&c->done, H2_ATOMIC_SEQ_CST);
       ++waited) {
    if (waited == 2000) return H2_PAL_ERR_TIMEOUT;
    int rc = h2_pal_time_sleep_ms(c->rt->time, 1);
    if (rc != 0) return rc;
  }
  if (c->task) {
    int rc = h2_pal_task_join(c->rt->task, c->task);
    if (rc != 0) return rc;
    c->task = NULL;
    if (c->cleanup_error != 0) return c->cleanup_error;
  }
  if (c->started || c->borrowed) {
    /* An explicit subsequent cleanup call retries ownership in a new worker. */
    c->cleanup_only = 1;
    h2_atomic_int_store(&c->done, 0, H2_ATOMIC_SEQ_CST);
    h2_pal_task_options_t options = {.name = "mfg/camera",
                                     .min_stack_size = 8192};
    int rc = h2_pal_task_start(c->rt->task, &options, camera_run, c, &c->task);
    if (rc != 0) h2_atomic_int_store(&c->done, 1, H2_ATOMIC_SEQ_CST);
    return rc != 0 ? rc : camera_worker_leave(m);
  }
  int rc = h2_pal_mutex_destroy(c->rt->sync, c->mutex);
  if (rc != 0) return rc;
  camera_worker_free(m);
  return 0;
}

static int scan_tick(h2_mfg_t* m, uint64_t now) {
  m->model.ready = 0;
  if (!m->scan_mutex) return H2_PAL_ERR_INVALID_STATE;
  uint32_t window =
      m->profile->scan_window_ms ? m->profile->scan_window_ms : 10000;
  int stopped = 0;
  if (m->scan_started && now - m->started_ms >= window) stopped = scan_stop(m);
  int rc = h2_pal_mutex_lock(m->rt->sync, m->scan_mutex);
  if (rc != 0) return rc;
  if (stopped != 0) m->scan_error = stopped;
  if (m->scan_done && !m->scan_count && !m->scan_error)
    m->scan_error = H2_PAL_ERR_TIMEOUT;
  rc = m->scan_error;
  m->model.ready = m->scan_done && m->scan_count != 0 && rc == 0;
  (void)snprintf(m->detail, sizeof(m->detail),
                 "BLE scan devices=%u done=%d error=%d manual=pending",
                 m->scan_count, m->scan_done, rc);
  (void)h2_pal_mutex_unlock(m->rt->sync, m->scan_mutex);
  return rc;
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
    case H2_MFG_BLE:
      return m->profile->ble_scan_only ? scan_tick(m, now) : 0;
    default:
      return 0;
  }
}
