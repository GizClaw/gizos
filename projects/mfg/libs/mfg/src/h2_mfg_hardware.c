#include "h2_mfg_internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

enum {
  AUDIO_POLL_BUDGET = 16,
  AUDIO_PLAYBACK_BUFFER_FRAMES = 8,
  AUDIO_TEST_VOLUME = 70
};

static unsigned test_volume(const h2_mfg_t* m) {
  return m->profile->speaker_test_volume == 0 ? AUDIO_TEST_VOLUME
                                              : m->profile->speaker_test_volume;
}

static int has_check(const h2_mfg_t* m, h2_mfg_check_t check) {
  for (uint32_t i = 0; i < m->profile->total; ++i)
    if (m->profile->steps[i].check == check) return 1;
  return 0;
}

static int storage(h2_mfg_t* m) {
  enum { SIZE = 65536, CHUNK = 512 };
  const char* path = "/data/.gizos-mfg-probe";
  uint8_t* data = h2_pal_mem_alloc(m->rt->mem, SIZE);
  if (data == NULL) return H2_PAL_ERR_NO_MEMORY;
  volatile uint8_t* ram = data;
  for (size_t i = 0; i < SIZE; ++i) ram[i] = (uint8_t)(i ^ (i >> 8) ^ 0xa5);
  int rc = 0;
  for (size_t i = 0; i < SIZE; ++i)
    if (ram[i] != (uint8_t)(i ^ (i >> 8) ^ 0xa5)) rc = H2_PAL_ERR_IO;
  h2_pal_fs_file_t* file = NULL;
  if (rc == 0)
    rc = h2_pal_fs_open(m->rt->fs, path, H2_PAL_FS_OPEN_WRITE_TRUNCATE, &file);
  size_t bytes = 0;
  if (rc == 0) rc = h2_pal_fs_write(m->rt->fs, file, data, CHUNK, &bytes);
  if (rc == 0 && bytes != CHUNK) rc = H2_PAL_ERR_IO;
  if (file != NULL) {
    int close_rc = h2_pal_fs_close(m->rt->fs, file);
    file = NULL;
    if (rc == 0) rc = close_rc;
  }
  uint8_t readback[CHUNK];
  if (rc == 0) rc = h2_pal_fs_open(m->rt->fs, path, H2_PAL_FS_OPEN_READ, &file);
  if (rc == 0) rc = h2_pal_fs_read(m->rt->fs, file, readback, CHUNK, &bytes);
  if (rc == 0 && (bytes != CHUNK || memcmp(data, readback, CHUNK) != 0))
    rc = H2_PAL_ERR_IO;
  if (file != NULL) {
    int close_rc = h2_pal_fs_close(m->rt->fs, file);
    if (rc == 0) rc = close_rc;
  }
  int remove_rc = h2_pal_fs_remove(m->rt->fs, path);
  if (rc == 0) rc = remove_rc;
  h2_pal_mem_free(m->rt->mem, data);
  (void)snprintf(m->detail, sizeof(m->detail),
                 "RAM pattern 64KiB filesystem readback 512B rc=%d", rc);
  return rc;
}

static int display_open(h2_mfg_t* m) {
  if (!has_check(m, H2_MFG_DISPLAY)) return 0;
  if (m->display_open && m->frame != NULL) return 0;
  if (m->display_open) {
    int close_rc = h2_pal_display_close(m->rt->display);
    if (close_rc != 0) return close_rc;
    m->display_open = 0;
  }
  int rc = h2_pal_display_open(m->rt->display);
  if (rc != 0) return rc;
  m->display_open = 1;
  rc = h2_pal_display_get_info(m->rt->display, &m->display);
  if (rc != 0) return rc;
  if (m->display.width < 120 || m->display.height < 120 ||
      m->display.width > 1024 || m->display.height > 1024)
    return H2_PAL_ERR_FORMAT;
  m->frame =
      h2_pal_mem_alloc(m->rt->mem, (size_t)m->display.width *
                                       m->display.height * sizeof(*m->frame));
  if (m->frame == NULL) return H2_PAL_ERR_NO_MEMORY;
  return h2_pal_display_set_brightness_percent(m->rt->display, 100);
}

static int audio_enter(h2_mfg_t* m, int mic) {
  int rc = h2_pal_audio_get_info(m->rt->audio, &m->audio_info);
  if (rc != 0) return rc;
  if (!m->audio_info.available ||
      !(mic ? m->audio_info.mic_supported : m->audio_info.playback_supported))
    return H2_PAL_ERR_UNSUPPORTED;
  h2_audio_pcm_format_t format =
      mic ? m->audio_info.mic_format : m->audio_info.playback_format;
  if (format.sample_format != H2_AUDIO_SAMPLE_S16LE || format.channels == 0 ||
      format.channels > 4 || format.sample_rate_hz == 0 ||
      format.frame_samples_per_channel == 0)
    return H2_PAL_ERR_FORMAT;
  m->pcm_bytes =
      h2_audio_pcm_frame_bytes(&format) * format.frame_samples_per_channel;
  if (m->pcm_bytes > 65536) return H2_PAL_ERR_FORMAT;
  m->pcm = h2_pal_mem_alloc(m->rt->mem, m->pcm_bytes);
  if (m->pcm == NULL) return H2_PAL_ERR_NO_MEMORY;
  if (mic) {
    rc = h2_pal_audio_start_mic(m->rt->audio);
    if (rc == 0) m->mic_started = 1;
  } else {
    rc =
        h2_pal_audio_get_speaker_volume_percent(m->rt->audio, &m->saved_volume);
    if (rc == 0) {
      m->volume_saved = 1;
      rc =
          h2_pal_audio_set_speaker_volume_percent(m->rt->audio, test_volume(m));
    }
    if (rc == 0) rc = h2_pal_audio_start_speaker(m->rt->audio);
    if (rc == 0) {
      m->speaker_started = 1;
      h2_audio_track_config_t config = {
          .name = "mfg/tone",
          .format = format,
          .volume_factor_milli = 1000,
          .buffer_frames = AUDIO_PLAYBACK_BUFFER_FRAMES,
          .allocator = m->rt->mem};
      rc = h2_pal_audio_create_track(m->rt->audio, &config, &m->track);
    }
  }
  return rc;
}

int h2_mfg_audio_stop(h2_mfg_t* m) {
  int rc;
  /* Tracks borrow the mixer: close them before stopping the speaker. On any
   * failed close/join retain the handles and backing storage for retry. */
  if (m->track != NULL) {
    rc = h2_pal_audio_track_close(m->track);
    if (rc != 0) return rc;
    m->track = NULL;
  }
  if (m->speaker_started) {
    rc = h2_pal_audio_stop_speaker(m->rt->audio);
    if (rc != 0) return rc;
    m->speaker_started = 0;
  }
  if (m->mic_started) {
    rc = h2_pal_audio_stop_mic(m->rt->audio);
    if (rc != 0) return rc;
    m->mic_started = 0;
  }
  return H2_PAL_OK;
}

int h2_mfg_hardware_leave(h2_mfg_t* m) {
  int rc = h2_mfg_peripheral_leave(m);
  if (rc != 0) return rc;
  rc = h2_mfg_audio_stop(m);
  if (rc != 0) return rc;
  rc = h2_mfg_aec_close(m);
  if (rc != 0) return rc;
  if (m->volume_saved) {
    rc = h2_pal_audio_set_speaker_volume_percent(m->rt->audio, m->saved_volume);
    if (rc != 0) return rc;
    m->volume_saved = 0;
  }
  h2_pal_mem_free(m->rt->mem, m->pcm);
  m->pcm = NULL;
  if (m->led_started) {
    rc = h2_pal_led_clear(m->rt->led, m->profile->led);
    if (rc != 0) return rc;
    m->led_started = 0;
  }
  return H2_PAL_OK;
}

int h2_mfg_hardware_close(h2_mfg_t* m) {
  int rc = h2_mfg_hardware_leave(m);
  if (rc != 0) return rc;
  if (m->touch_open) {
    rc = h2_pal_touch_close(m->rt->touch);
    if (rc != 0) return rc;
    m->touch_open = 0;
  }
  if (m->display_open) {
    rc = h2_pal_display_close(m->rt->display);
    if (rc != 0) return rc;
    m->display_open = 0;
  }
  h2_pal_mem_free(m->rt->mem, m->frame);
  m->frame = NULL;
  return H2_PAL_OK;
}

int h2_mfg_hardware_enter(h2_mfg_t* m, uint64_t now) {
  if ((m->profile->touch_points == NULL) !=
          (m->profile->touch_point_count == 0) ||
      m->profile->touch_point_count > 32 || m->profile->accel_delta_mg < 0 ||
      m->profile->gyro_delta_mdps < 0 ||
      (m->profile->imu_required_flags &
       ~(H2_PAL_IMU_HAS_ACCEL | H2_PAL_IMU_HAS_GYRO)) ||
      (m->profile->hold_vibration && (m->profile->vibration_percent == 0 ||
                                      m->profile->vibration_percent > 100)))
    return H2_PAL_ERR_INVALID_ARG;
  m->imu_baseline_valid = m->accel_responded = m->gyro_responded = 0;
  m->vibration_requested = m->vibration_observed = 0;
  m->started_ms = now;
  m->touch_hits = m->touch_releases = m->touch_downs = 0;
  m->touch_pressed = m->touch_action = 0;
  memset(m->downs, 0, sizeof(m->downs));
  memset(m->ups, 0, sizeof(m->ups));
  memset(m->armed, 0, sizeof(m->armed));
  m->samples = m->audio_frames = m->mic_peak = m->mic_level = m->phase = 0;
  m->audio_samples = m->mic_energy = 0;
  m->mic_level_ms = now;
  m->ble_probed = 0;
  for (unsigned i = 0; i < 2; ++i) {
    m->imu_min[i] = INT32_MAX;
    m->imu_max[i] = INT32_MIN;
  }
  m->detail[0] = '\0';
  m->model.ready = 0;
  if (m->model.current >= m->profile->total) return display_open(m);
  h2_mfg_check_t check = m->profile->steps[m->model.current].check;
  int rc = check == H2_MFG_STORAGE ? 0 : display_open(m);
  if (rc != 0 && (check == H2_MFG_DISPLAY || check == H2_MFG_TOUCH)) return rc;
  rc = 0;
  if (m->profile->touch_points && m->display_open) {
    for (uint32_t i = 0; i < m->profile->touch_point_count; ++i) {
      const h2_mfg_touch_point_t* point = &m->profile->touch_points[i];
      if (point->x < 0 || point->y < 0 || point->x >= m->display.width ||
          point->y >= m->display.height)
        return H2_PAL_ERR_INVALID_ARG;
    }
  }
  /* The screen controls need Touch on every resumed check, including a
   * persisted failure. A cold boot may never enter the earlier TOUCH step. */
  if (has_check(m, H2_MFG_TOUCH) && m->display_open && !m->touch_open) {
    rc = h2_pal_touch_open(m->rt->touch);
    if (rc == 0) {
      m->touch_open = 1;
      h2_pal_touch_info_t info = {0};
      rc = h2_pal_touch_get_info(m->rt->touch, &info);
      if (rc == 0 && (info.width != (uint32_t)m->display.width ||
                      info.height != (uint32_t)m->display.height))
        rc = H2_PAL_ERR_FORMAT;
    }
  }
  if (rc != 0 && check == H2_MFG_TOUCH) return rc;
  rc = 0;
  if (m->model.summary.step_status[m->model.current] ==
      H2_LOADER_MFG_STEP_FAILED) {
    (void)snprintf(m->detail, sizeof(m->detail), "Failed check requires retry");
    return 0;
  }
  if (check == H2_MFG_SPEAKER || check == H2_MFG_MIC)
    rc = audio_enter(m, check == H2_MFG_MIC);
  if (check == H2_MFG_AEC) rc = h2_mfg_aec_enter(m);
  if (rc == 0) rc = h2_mfg_peripheral_enter(m);
  return rc;
}

static int buttons(h2_mfg_t* m, const char** action) {
  h2_pal_radio_button_group_reading_t group = {0};
  if (m->profile->button_group != 0) {
    int rc = h2_pal_button_read_radio_button_group(
        m->rt->button, m->profile->button_group, &group);
    if (rc != 0) return rc;
  }
  for (unsigned i = 0; i < m->profile->button_count; ++i) {
    h2_pal_single_button_reading_t reading = {0};
    int rc = 0;
    if (m->profile->button_group != 0)
      reading.state = group.pressed_button_id == m->profile->buttons[i]
                          ? H2_PAL_BUTTON_STATE_PRESSED
                          : H2_PAL_BUTTON_STATE_RELEASED;
    else
      rc = h2_pal_button_read_single_button(m->rt->button,
                                            m->profile->buttons[i], &reading);
    if (rc != 0) return rc;
    int pressed = reading.state == H2_PAL_BUTTON_STATE_PRESSED;
    if (pressed == m->pressed[i]) continue;
    m->pressed[i] = pressed;
    if (pressed) {
      ++m->downs[i];
      m->armed[i] = m->model.ready || m->model.error != 0 ||
                    (m->model.current < m->profile->total &&
                     m->model.summary.step_status[m->model.current] ==
                         H2_LOADER_MFG_STEP_FAILED);
    } else {
      ++m->ups[i];
      if (i == 0 && m->armed[i]) *action = "confirm";
      m->armed[i] = 0;
    }
  }
  return 0;
}

static int rect_hit(const h2_mfg_ui_rect_t* r, int x, int y) {
  return r->width > 0 && r->height > 0 && x >= r->x && y >= r->y &&
         x - r->x < r->width && y - r->y < r->height;
}
static unsigned controls_hit(const h2_mfg_ui_config_t* ui, int x, int y) {
  if (rect_hit(&ui->controls[0], x, y)) return 2;
  if (rect_hit(&ui->controls[1], x, y)) return 1;
  return 0;
}
static int vibration_hit(const h2_mfg_t* m, int x, int y) {
  return m->profile->ui != NULL && rect_hit(&m->profile->ui->vibration, x, y);
}

static int touch(h2_mfg_t* m, h2_mfg_check_t check, const char** action) {
  if (!m->touch_open) return 0;
  h2_pal_touch_event_t event = {0};
  for (unsigned budget = 0; budget < 16; ++budget) {
    int rc = h2_pal_touch_poll_event(m->rt->touch, &event);
    if (rc == H2_PAL_ERR_WOULD_BLOCK || rc == H2_PAL_ERR_TIMEOUT) return 0;
    if (rc != 0) return rc;
    if (event.x < 0 || event.y < 0 || event.x >= m->display.width ||
        event.y >= m->display.height)
      return H2_PAL_ERR_FORMAT;
    m->touch_x = event.x;
    m->touch_y = event.y;
    if (event.kind == H2_PAL_TOUCH_EVENT_DOWN) {
      ++m->touch_downs;
      m->touch_pressed = 1;
      m->touch_action = event.y > m->display.height * 82 / 100
                            ? ((uint32_t)event.x < m->display.width / 2u
                                   ? (m->model.ready ? 1u : 3u)
                                   : 2u)
                            : 0u;
      if (m->profile->ui != NULL) {
        unsigned hit = controls_hit(m->profile->ui, event.x, event.y);
        m->touch_action = hit == 1 && !m->model.ready ? 3u : hit;
      }
      if (m->profile->hold_vibration && check == H2_MFG_VIBRATION &&
          vibration_hit(m, event.x, event.y))
        m->vibration_requested = 1;
    }
    if (event.kind == H2_PAL_TOUCH_EVENT_UP) {
      ++m->touch_releases;
      m->touch_pressed = 0;
      m->vibration_requested = 0;
      unsigned hit = m->profile->ui != NULL
                         ? controls_hit(m->profile->ui, event.x, event.y)
                     : event.y > m->display.height * 82 / 100
                         ? ((uint32_t)event.x < m->display.width / 2u ? 1u : 2u)
                         : 0u;
      if (hit != 0) {
        if (m->touch_action == hit) *action = hit == 1 ? "confirm" : "fail";
        if (hit == 1u && m->touch_action == 3u &&
            (m->model.error != 0 ||
             m->model.summary.step_status[m->model.current] ==
                 H2_LOADER_MFG_STEP_FAILED))
          *action = "retry";
        if (*action != NULL) {
          char line[128];
          (void)snprintf(line, sizeof(line),
                         "H2_MFG_UI_TOUCH action=%s x=%" PRId32 " y=%" PRId32
                         " ticket=%08" PRIx32,
                         *action, event.x, event.y, m->model.ticket);
          (void)h2_pal_log_write(m->rt->log, H2_PAL_LOG_INFO, "mfg", line);
        }
      }
      m->touch_action = 0;
    } else if (check == H2_MFG_TOUCH) {
      unsigned count =
          m->profile->touch_point_count ? m->profile->touch_point_count : 5u;
      for (unsigned i = 0; i < count; ++i) {
        int x, y;
        h2_mfg_touch_target(m, i, &x, &y);
        int dx = event.x - x, dy = event.y - y;
        int radius = (int)m->display.width / 20;
        if (dx * dx + dy * dy <= radius * radius) m->touch_hits |= 1u << i;
      }
    }
    if (m->profile->hold_vibration && event.kind == H2_PAL_TOUCH_EVENT_MOVE &&
        !vibration_hit(m, event.x, event.y))
      m->vibration_requested = 0;
  }
  return 0;
}

static int32_t axis(const h2_pal_imu_reading_t* reading, int which) {
  int32_t xyz[] = {reading->accel_mg.x, reading->accel_mg.y,
                   reading->accel_mg.z};
  unsigned index = (unsigned)(which < 0 ? -which : which);
  if (index == 0 || index > 3) return 0;
  return which < 0 ? -xyz[index - 1] : xyz[index - 1];
}

int h2_mfg_audio_tick(h2_mfg_t* m, uint64_t now, int mic) {
  h2_audio_pcm_format_t format =
      mic ? m->audio_info.mic_format : m->audio_info.playback_format;
  h2_audio_frame_t frame =
      h2_audio_frame_for_buffer(m->pcm, m->pcm_bytes, format);
  int rc;
  if (mic) {
    unsigned received = 0, peak = 0;
    /* A display transfer can take several PCM periods. Drain the queued
     * capture instead of reading only one frame per UI iteration. */
    for (unsigned budget = 0; budget < AUDIO_POLL_BUDGET; ++budget) {
      rc = h2_pal_audio_mic_read(m->rt->audio, &frame, 0);
      if (rc == H2_PAL_ERR_WOULD_BLOCK || rc == H2_PAL_ERR_TIMEOUT) break;
      if (rc != 0) return rc;
      if (frame.bytes == 0 || frame.bytes > m->pcm_bytes ||
          frame.bytes % h2_audio_pcm_frame_bytes(&format) != 0)
        return H2_PAL_ERR_FORMAT;
      rc = h2_mfg_aec_mic_frame(m, &frame);
      if (rc != 0) return rc;
      int16_t* pcm = m->pcm;
      for (size_t i = 0; i < frame.bytes / sizeof(*pcm); ++i) {
        int value = pcm[i];
        unsigned magnitude = (unsigned)(value < 0 ? -value : value);
        if (magnitude > peak) peak = magnitude;
        if (magnitude > m->mic_peak) m->mic_peak = magnitude;
        m->mic_energy += (uint64_t)(value * value);
      }
      ++received;
      ++m->audio_frames;
    }
    if (received != 0) {
      m->mic_level = peak;
      m->mic_level_ms = now;
    } else if (now - m->mic_level_ms >= 250) {
      m->mic_level = 0;
    }
    m->model.ready =
        m->audio_frames >= 40 && m->mic_peak >= 128 && m->mic_energy >= 1000000;
    (void)snprintf(m->detail, sizeof(m->detail),
                   "Speak near mic frames=%u level=%u peak=%u energy=%" PRIu64,
                   m->audio_frames, m->mic_level, m->mic_peak, m->mic_energy);
    return 0;
  }
  /* Keep a bounded playback queue full before drawing the screen. One
   * 32-ms frame per ~125-ms UI iteration starves the DAC. The provider's
   * backpressure paces playback; never advance the tone on a rejected write. */
  for (unsigned budget = 0; budget < AUDIO_POLL_BUDGET; ++budget) {
    int16_t* pcm = m->pcm;
    for (unsigned i = 0; i < format.frame_samples_per_channel; ++i) {
      uint64_t position = m->audio_samples + i;
      int on =
          position % (format.sample_rate_hz * 2u) < format.sample_rate_hz / 2u;
      /* Triangle wave avoids libm and sharp square-wave harmonics. */
      unsigned p =
          (unsigned)((position * 440u * 256u / format.sample_rate_hz) & 255u);
      int value = (p < 128 ? (int)p : 255 - (int)p) * 128 - 8192;
      for (unsigned lane = 0; lane < format.channels; ++lane)
        pcm[i * format.channels + lane] = (int16_t)(on ? value : 0);
    }
    frame.bytes = m->pcm_bytes;
    rc = h2_pal_audio_track_write(m->track, &frame, 0);
    if (rc == H2_PAL_ERR_WOULD_BLOCK || rc == H2_PAL_ERR_TIMEOUT) break;
    if (rc != 0) return rc;
    m->audio_samples += format.frame_samples_per_channel;
    ++m->audio_frames;
  }
  m->model.ready = m->audio_samples >= format.sample_rate_hz;
  (void)snprintf(m->detail, sizeof(m->detail),
                 "Confirm audible tone no distortion frames=%u pcm_ms=%" PRIu64
                 " elapsed_ms=%" PRIu64 " volume=%u",
                 m->audio_frames,
                 m->audio_samples * 1000u / format.sample_rate_hz,
                 now - m->started_ms, test_volume(m));
  return 0;
}

int h2_mfg_hardware_tick(h2_mfg_t* m, uint64_t now, const char** action) {
  if (m->model.current >= m->profile->total) return 0;
  const h2_mfg_step_t* step = &m->profile->steps[m->model.current];
  int failed =
      m->model.error != 0 || m->model.summary.step_status[m->model.current] ==
                                 H2_LOADER_MFG_STEP_FAILED;
  int rc = buttons(m, action);
  if (rc != 0 && step->check != H2_MFG_BUTTONS) rc = 0;
  int touch_rc = touch(m, step->check, action);
  if (touch_rc != 0 && step->check == H2_MFG_TOUCH) rc = touch_rc;
  if (failed) {
    if (*action != NULL && strcmp(*action, "confirm") == 0) *action = "retry";
    return 0;
  }
  if (rc != 0) return rc;
  switch (step->check) {
    case H2_MFG_STORAGE:
      rc = storage(m);
      m->model.ready = rc == 0;
      break;
    case H2_MFG_DISPLAY:
      if (now - m->started_ms < 600)
        rc = h2_pal_display_set_brightness_percent(m->rt->display, 100);
      else if (now - m->started_ms < 1200)
        rc = h2_pal_display_set_brightness_percent(m->rt->display, 50);
      else
        rc = h2_pal_display_set_brightness_percent(m->rt->display, 100);
      m->model.ready = now - m->started_ms >= 1800;
      (void)snprintf(m->detail, sizeof(m->detail),
                     "Check red green blue white and brightness");
      break;
    case H2_MFG_TOUCH: {
      unsigned count =
          m->profile->touch_point_count ? m->profile->touch_point_count : 5u;
      uint32_t mask = count == 32 ? UINT32_MAX : (UINT32_C(1) << count) - 1u;
      m->model.ready = m->touch_hits == mask && m->touch_downs >= count &&
                       m->touch_releases >= count && !m->touch_pressed;
      (void)snprintf(m->detail, sizeof(m->detail),
                     "Touch five crosses mask=%02x down=%u up=%u",
                     m->touch_hits, m->touch_downs, m->touch_releases);
      break;
    }
    case H2_MFG_BUTTONS:
      m->model.ready = m->profile->button_count > 0;
      for (unsigned i = 0; i < m->profile->button_count; ++i)
        if (m->downs[i] < 3 || m->ups[i] < 3 || m->pressed[i])
          m->model.ready = 0;
      if (m->profile->has_reset_button || m->profile->has_power_button)
        (void)snprintf(
            m->detail, sizeof(m->detail),
            "BOOT=%u/%u%s%s operator_confirm=pending", m->downs[0], m->ups[0],
            m->profile->has_power_button ? " POWER=hardware-switch" : "",
            m->profile->has_reset_button ? " RESET=hardware-reboot" : "");
      else
        (void)snprintf(m->detail, sizeof(m->detail),
                       "Each button three cycles BOOT=%u/%u PWR=%u/%u",
                       m->downs[0], m->ups[0], m->downs[1], m->ups[1]);
      break;
    case H2_MFG_IMU: {
      rc = h2_pal_imu_read(m->rt->imu, m->profile->imu, &m->imu);
      if (rc != 0) break;
      if (m->profile->accel_delta_mg > 0 || m->profile->gyro_delta_mdps > 0) {
        const uint32_t channels = H2_PAL_IMU_HAS_ACCEL | H2_PAL_IMU_HAS_GYRO;
        uint32_t available = m->imu.flags & channels;
        uint32_t enabled =
            (m->profile->accel_delta_mg > 0 ? H2_PAL_IMU_HAS_ACCEL : 0) |
            (m->profile->gyro_delta_mdps > 0 ? H2_PAL_IMU_HAS_GYRO : 0);
        uint32_t required = m->profile->imu_required_flags
                                ? m->profile->imu_required_flags
                                : available & enabled;
        if ((required & enabled) != required) return H2_PAL_ERR_INVALID_ARG;
        if (required == 0 || (available & required) != required)
          return H2_PAL_ERR_UNSUPPORTED;
        int32_t values[] = {m->imu.accel_mg.x,  m->imu.accel_mg.y,
                            m->imu.accel_mg.z,  m->imu.gyro_mdps.x,
                            m->imu.gyro_mdps.y, m->imu.gyro_mdps.z};
        if (!m->imu_baseline_valid) {
          memcpy(m->imu_baseline, values, sizeof(values));
          m->imu_baseline_valid = 1;
        } else
          for (unsigned i = 0; i < 6; ++i) {
            int64_t delta = (int64_t)values[i] - m->imu_baseline[i];
            if (delta < 0) delta = -delta;
            if (i < 3 && (required & H2_PAL_IMU_HAS_ACCEL) &&
                m->profile->accel_delta_mg > 0)
              m->accel_responded |= delta >= m->profile->accel_delta_mg;
            if (i >= 3 && (required & H2_PAL_IMU_HAS_GYRO) &&
                m->profile->gyro_delta_mdps > 0)
              m->gyro_responded |= delta >= m->profile->gyro_delta_mdps;
          }
        m->model.ready =
            (!(required & H2_PAL_IMU_HAS_ACCEL) || m->accel_responded) &&
            (!(required & H2_PAL_IMU_HAS_GYRO) || m->gyro_responded);
        break;
      }
      ++m->samples;
      int32_t xy[] = {axis(&m->imu, m->profile->screen_x_axis),
                      axis(&m->imu, m->profile->screen_y_axis)};
      for (unsigned i = 0; i < 2; ++i) {
        if (xy[i] < m->imu_min[i]) m->imu_min[i] = xy[i];
        if (xy[i] > m->imu_max[i]) m->imu_max[i] = xy[i];
      }
      m->model.ready = m->samples >= 20 &&
                       (int64_t)m->imu_max[0] - m->imu_min[0] >= 400 &&
                       (int64_t)m->imu_max[1] - m->imu_min[1] >= 400;
      (void)snprintf(m->detail, sizeof(m->detail),
                     "Tilt both axes X=%" PRId32 " Y=%" PRId32 " samples=%u",
                     xy[0], xy[1], m->samples);
      break;
    }
    case H2_MFG_SPEAKER:
      rc = h2_mfg_audio_tick(m, now, 0);
      break;
    case H2_MFG_MIC:
      rc = h2_mfg_audio_tick(m, now, 1);
      break;
    case H2_MFG_AEC:
      rc = h2_mfg_aec_tick(m);
      break;
    case H2_MFG_MAGNETOMETERS:
    case H2_MFG_BATTERY:
    case H2_MFG_VIBRATION:
    case H2_MFG_CAMERA:
      rc = h2_mfg_peripheral_tick(m, now);
      break;
    case H2_MFG_WIFI: {
      h2_pal_wifi_sta_status_t wifi = {0};
      rc = h2_pal_wifi_sta_get_status(m->rt->wifi_sta, &wifi);
      m->model.ready = rc == 0 && h2_pal_wifi_sta_status_has_ip(&wifi);
      (void)snprintf(m->detail, sizeof(m->detail),
                     "Connect with Loader wifi command IP=%d", m->model.ready);
      break;
    }
    case H2_MFG_BLE:
      m->model.ready = m->ble_probed;
      if (!m->ble_probed)
        (void)snprintf(m->detail, sizeof(m->detail),
                       "Board peer send mfg probe ble TICKET over BLE");
      break;
    case H2_MFG_LED: {
      h2_pal_led_color_t color = {0};
      unsigned phase = (unsigned)((now - m->started_ms) / 700u) % 3u;
      if (phase == 0) color.r = 32;
      if (phase == 1) color.g = 32;
      if (phase == 2) color.b = 32;
      rc = h2_pal_led_set_solid(m->rt->led, m->profile->led, color);
      if (rc == 0) m->led_started = 1;
      m->model.ready = now - m->started_ms >= 2100;
      (void)snprintf(m->detail, sizeof(m->detail), "Confirm RGB LED sequence");
      break;
    }
    case H2_MFG_MODEM: {
      h2_pal_modem_identity_t identity = {0};
      rc = h2_pal_modem_get_identity(m->rt->modem, &identity);
      m->model.ready =
          rc == 0 && identity.model[0] != '\0' && identity.imei[0] != '\0';
      (void)snprintf(m->detail, sizeof(m->detail),
                     "Modem identity response valid=%d", m->model.ready);
      break;
    }
  }
  if (rc == 0 && step->automatic && m->model.ready) *action = "confirm";
  return rc;
}
