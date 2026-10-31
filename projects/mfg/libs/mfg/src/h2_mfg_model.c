#include "h2_mfg_model.h"

#include <string.h>

static uint32_t first_pending(const h2_loader_mfg_summary_t* summary) {
  for (uint32_t i = 0; i < summary->total; ++i)
    if (summary->step_status[i] != H2_LOADER_MFG_STEP_PASSED) return i;
  return summary->total;
}

static int ui_rect_valid(const h2_mfg_ui_config_t* ui,
                         const h2_mfg_ui_rect_t* r) {
  return r->x >= 0 && r->y >= 0 && r->width > 0 && r->height > 0 &&
         (uint32_t)r->x < ui->width && (uint32_t)r->y < ui->height &&
         (uint32_t)r->width <= ui->width - (uint32_t)r->x &&
         (uint32_t)r->height <= ui->height - (uint32_t)r->y;
}

static int ui_valid(const h2_mfg_profile_t* profile) {
  const h2_mfg_ui_config_t* ui = profile->ui;
  if (ui == NULL) return !profile->sequential_touch_grid;
  if (ui->width < 120 || ui->height < 120 || ui->width > 1024 ||
      ui->height > 1024 || ui->title == NULL || ui->touch_tolerance > 1024)
    return 0;
  const h2_mfg_ui_rect_t* rects[] = {&ui->title_area,
                                     &ui->heading,
                                     &ui->progress,
                                     &ui->controls[0],
                                     &ui->controls[1],
                                     &ui->retest,
                                     &ui->display_pattern,
                                     &ui->display_note,
                                     &ui->brightness_levels,
                                     &ui->brightness_control,
                                     &ui->brightness_note,
                                     &ui->vibration,
                                     &ui->preview};
  for (unsigned i = 0; i < sizeof(rects) / sizeof(rects[0]); ++i)
    if (!ui_rect_valid(ui, rects[i])) return 0;
  for (unsigned i = 0; i < 8; ++i)
    if (!ui_rect_valid(ui, &ui->rows[i])) return 0;
  if (profile->sequential_touch_grid)
    for (unsigned i = 0; i < 9; ++i)
      if (!ui_rect_valid(ui, &ui->touch_targets[i])) return 0;
  for (unsigned i = 0; i < profile->magnetometer_count; ++i) {
    if (ui->magnet_order[i] >= profile->magnetometer_count ||
        !ui->magnet_names[i] || !ui_rect_valid(ui, &ui->magnet_boxes[i]) ||
        !ui_rect_valid(ui, &ui->magnet_labels[i]) ||
        !ui_rect_valid(ui, &ui->magnet_errors[i]))
      return 0;
    for (unsigned j = 0; j < i; ++j)
      if (ui->magnet_order[j] == ui->magnet_order[i]) return 0;
  }
  return 1;
}

int h2_mfg_model_init(h2_mfg_model_t* m, const h2_mfg_profile_t* profile,
                      const h2_loader_mfg_summary_t* summary, uint32_t ticket,
                      h2_mfg_save_fn save, void* save_user) {
  if (m == NULL || profile == NULL || summary == NULL || save == NULL ||
      profile->board == NULL || profile->steps == NULL || profile->total == 0 ||
      profile->total > H2_LOADER_MFG_STEP_MAX || profile->revision == 0 ||
      profile->button_count > H2_MFG_BUTTON_MAX ||
      profile->magnetometer_count > H2_MFG_MAGNETOMETER_MAX ||
      profile->speaker_test_volume > 100 || summary->total != profile->total ||
      ticket == 0)
    return H2_PAL_ERR_INVALID_ARG;
  if ((profile->sequential_touch_grid && !profile->touch_navigation) ||
      (profile->aec_live_monitor && profile->monitor_min_frames == 0) ||
      (profile->live_magnets && profile->magnetic_delta_mgauss <= 0) ||
      (profile->hold_vibration &&
       (profile->vibration_percent == 0 || profile->vibration_percent > 100)) ||
      (profile->ble_scan_only && profile->scan_window_ms == 0) ||
      (profile->preview_width == 0) != (profile->preview_height == 0) ||
      profile->preview_width > 1024 || profile->preview_height > 1024 ||
      profile->accel_delta_mg < 0 || profile->gyro_delta_mdps < 0)
    return H2_PAL_ERR_INVALID_ARG;
  if (!ui_valid(profile) ||
      (profile->wifi_test_config != NULL &&
       (h2_pal_wifi_settings_validate_sta_config(profile->wifi_test_config) !=
            0 ||
        profile->wifi_timeout_ms == 0 || profile->wifi_timeout_ms > 60000)))
    return H2_PAL_ERR_INVALID_ARG;
  int microphone = 0, speaker = 0, aec = 0;
  for (uint32_t i = 0; i < H2_LOADER_MFG_STEP_MAX; ++i) {
    if (summary->step_status[i] > H2_LOADER_MFG_STEP_FAILED ||
        (i >= summary->total && summary->step_status[i] != 0))
      return H2_PAL_ERR_FORMAT;
    if (i < profile->total) {
      microphone |= profile->steps[i].check == H2_MFG_MIC;
      speaker |= profile->steps[i].check == H2_MFG_SPEAKER;
      aec |= profile->steps[i].check == H2_MFG_AEC;
      if (profile->steps[i].check == H2_MFG_AEC && profile->steps[i].automatic)
        return H2_PAL_ERR_INVALID_ARG;
      if ((profile->steps[i].check == H2_MFG_MAGNETOMETERS &&
           profile->magnetometer_count == 0) ||
          (profile->steps[i].check == H2_MFG_BATTERY && !profile->battery) ||
          (profile->steps[i].check == H2_MFG_VIBRATION && !profile->motor))
        return H2_PAL_ERR_INVALID_ARG;
      if ((profile->steps[i].check == H2_MFG_BRIGHTNESS &&
           !profile->touch_navigation) ||
          (profile->steps[i].check == H2_MFG_AEC &&
           !profile->aec_live_monitor && profile->monitor_min_frames != 0) ||
          (profile->steps[i].automatic &&
           ((profile->steps[i].check == H2_MFG_WIFI &&
             profile->wifi_manual_ipv4) ||
            (profile->steps[i].check == H2_MFG_BLE && profile->ble_scan_only) ||
            (profile->steps[i].check == H2_MFG_BATTERY &&
             profile->battery_manual_charge))))
        return H2_PAL_ERR_INVALID_ARG;
      if (profile->steps[i].name == NULL || profile->steps[i].name[0] == '\0')
        return H2_PAL_ERR_INVALID_ARG;
      for (uint32_t j = 0; j < i; ++j)
        if (strcmp(profile->steps[i].name, profile->steps[j].name) == 0)
          return H2_PAL_ERR_INVALID_ARG;
    }
  }
  if (microphone && speaker && !aec) return H2_PAL_ERR_INVALID_ARG;
  memset(m, 0, sizeof(*m));
  m->profile = profile;
  m->summary = *summary;
  m->current = first_pending(summary);
  m->ticket = ticket;
  m->save = save;
  m->save_user = save_user;
  return H2_PAL_OK;
}

int h2_mfg_model_complete(const h2_mfg_model_t* m) {
  return m != NULL && first_pending(&m->summary) == m->summary.total;
}

int h2_mfg_model_action(h2_mfg_model_t* m, const char* action, const char* step,
                        uint32_t ticket) {
  h2_loader_mfg_summary_t next = {0};
  uint32_t selected = 0;
  if (m == NULL || action == NULL || step == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (ticket != m->ticket) return H2_PAL_ERR_INVALID_STATE;
  next = m->summary;
  selected = m->current;
  if (strcmp(action, "reset") == 0) {
    if (strcmp(step, "all") != 0) return H2_PAL_ERR_INVALID_ARG;
    memset(next.step_status, 0, sizeof(next.step_status));
    selected = 0;
  } else if (strcmp(action, "retry") == 0) {
    for (selected = 0; selected < m->profile->total; ++selected)
      if (strcmp(m->profile->steps[selected].name, step) == 0) break;
    if (selected == m->profile->total) return H2_PAL_ERR_NOT_FOUND;
    next.step_status[selected] = H2_LOADER_MFG_STEP_UNTESTED;
  } else {
    if (selected >= m->profile->total ||
        strcmp(m->profile->steps[selected].name, step) != 0)
      return H2_PAL_ERR_INVALID_STATE;
    if (strcmp(action, "confirm") == 0) {
      if (!m->ready || m->error != H2_PAL_OK ||
          next.step_status[selected] == H2_LOADER_MFG_STEP_FAILED)
        return H2_PAL_ERR_INVALID_STATE;
      next.step_status[selected] = H2_LOADER_MFG_STEP_PASSED;
      selected = first_pending(&next);
      if (m->profile->sequential_traversal) {
        selected = m->current + 1u;
        while (selected < next.total &&
               next.step_status[selected] == H2_LOADER_MFG_STEP_PASSED)
          ++selected;
      }
    } else if (strcmp(action, "fail") == 0) {
      next.step_status[selected] = H2_LOADER_MFG_STEP_FAILED;
      if (m->profile->sequential_traversal) {
        ++selected;
        while (selected < next.total &&
               next.step_status[selected] == H2_LOADER_MFG_STEP_PASSED)
          ++selected;
      }
    } else if (strcmp(action, "skip") == 0) {
      next.step_status[selected] = H2_LOADER_MFG_STEP_SKIPPED;
      /* Review skipped/failed checks using retry; skipping never opens gate. */
      selected++;
      while (selected < next.total &&
             next.step_status[selected] == H2_LOADER_MFG_STEP_PASSED)
        selected++;
    } else {
      return H2_PAL_ERR_INVALID_ARG;
    }
  }
  int rc = m->save(m->save_user, &next);
  if (rc != H2_PAL_OK) return rc;
  m->summary = next;
  m->current = selected;
  m->ready = 0;
  m->error = H2_PAL_OK;
  if (++m->ticket == 0) ++m->ticket;
  return H2_PAL_OK;
}
