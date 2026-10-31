#include "h2_mfg_internal.h"
#include "h2_mfg_font.h"

#include <stdio.h>
#include <string.h>

enum {
  BG = 0x0862,
  WHITE = 0xffff,
  GREEN = 0x07e0,
  RED = 0xf800,
  BLUE = 0x153f,
  GREY = 0x4208
};

static void rect(h2_mfg_t* m, int x, int y, int w, int h, uint16_t color) {
  for (int row = y; row < y + h; ++row)
    for (int col = x; col < x + w; ++col)
      if (row >= 0 && col >= 0 && row < m->display.height &&
          col < m->display.width)
        m->frame[(size_t)row * m->display.width + col] = color;
}

static void disk(h2_mfg_t* m, int x, int y, int r, uint16_t color) {
  for (int dy = -r; dy <= r; ++dy)
    for (int dx = -r; dx <= r; ++dx)
      if (dx * dx + dy * dy <= r * r) rect(m, x + dx, y + dy, 1, 1, color);
}

static void text(h2_mfg_t* m, const char* s, int x, int y, int scale,
                 uint16_t color) {
  for (size_t i = 0; s[i] != '\0'; ++i) {
    int c = (unsigned char)s[i];
    if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    const uint8_t* glyph = c >= 'A' && c <= 'Z'   ? font[c - 'A']
                           : c >= '0' && c <= '9' ? font[26 + c - '0']
                                                  : NULL;
    for (int row = 0; row < 7; ++row) {
      uint8_t bits = glyph == NULL ? 0 : glyph[row];
      if ((c == '-' || c == '_') && row == 3) bits = 14;
      if (c == '/' && row < 5) bits = (uint8_t)(1u << row);
      for (int col = 0; col < 5; ++col)
        if (bits & (1u << (4 - col)))
          rect(m, x + ((int)i * 6 + col) * scale, y + row * scale, scale, scale,
               color);
    }
  }
}

static void center(h2_mfg_t* m, const char* s, int y, int scale,
                   uint16_t color) {
  text(m, s, (m->display.width - (int)strlen(s) * 6 * scale) / 2, y, scale,
       color);
}

void h2_mfg_touch_target(const h2_mfg_t* m, unsigned i, int* x, int* y) {
  static const int xy[5][2] = {
      {21, 25}, {79, 25}, {50, 50}, {21, 75}, {79, 75}};
  if (m->profile->sequential_touch_grid && m->profile->ui != NULL) {
    const h2_mfg_ui_rect_t* target = &m->profile->ui->touch_targets[i % 9u];
    *x = target->x + target->width / 2;
    *y = target->y + target->height / 2;
  } else {
    *x = m->display.width * xy[i % 5u][0] / 100;
    *y = m->display.height * xy[i % 5u][1] / 100;
  }
}

static int clamp(int value, int radius) {
  return value < -radius ? -radius : value > radius ? radius : value;
}

static int imu_axis(h2_mfg_t* m, int axis) {
  int32_t xyz[] = {m->imu.accel_mg.x, m->imu.accel_mg.y, m->imu.accel_mg.z};
  int index = axis < 0 ? -axis : axis;
  if (index < 1 || index > 3) return 0;
  return (int)(xyz[index - 1] / 20) * (axis < 0 ? -1 : 1);
}

/* Factory presentation owns pixels only. Input, readiness, and acceptance stay
 * in the shared controller/model. Coordinates match its touch hit regions. */
static void outline(h2_mfg_t* m, int x, int y, int w, int h, uint16_t color) {
  if (w <= 0 || h <= 0) return;
  int thickness = w < h ? w : h;
  if (thickness > 3) thickness = 3;
  rect(m, x, y, w, thickness, color);
  rect(m, x, y + h - thickness, w, thickness, color);
  rect(m, x, y, thickness, h, color);
  rect(m, x + w - thickness, y, thickness, h, color);
}

/* Bounded UTF-8 decoding also handles truncated/malformed advertising names.
 * Consume one byte on error, so an invalid peer name cannot hide later text. */
static uint32_t codepoint(const char** cursor) {
  const unsigned char* s = (const unsigned char*)*cursor;
  uint32_t c = *s++;
  unsigned tail = c < 0x80                 ? 0
                  : c >= 0xc2 && c <= 0xdf ? 1
                  : c >= 0xe0 && c <= 0xef ? 2
                  : c >= 0xf0 && c <= 0xf4 ? 3
                                           : 4;
  if (tail == 4) {
    ++*cursor;
    return '?';
  }
  uint32_t value = tail == 0 ? c : c & ((1u << (6u - tail)) - 1u);
  for (unsigned i = 0; i < tail; ++i) {
    if (s[i] == 0 || (s[i] & 0xc0u) != 0x80u) {
      ++*cursor;
      return '?';
    }
    value = (value << 6) | (s[i] & 0x3fu);
  }
  *cursor = (const char*)(s + tail);
  if ((tail == 1 && value < 0x80) || (tail == 2 && value < 0x800) ||
      (tail == 3 && value < 0x10000) || value > 0x10ffff ||
      (value >= 0xd800 && value <= 0xdfff))
    return '?';
  return value;
}

static int factory_glyph(uint32_t c, lv_font_glyph_dsc_t* glyph) {
  if (lv_font_get_glyph_dsc(&h2_mfg_factory_font, glyph, c, 0) &&
      !glyph->is_placeholder)
    return 1;
  lv_font_glyph_release_draw_data(glyph);
  return lv_font_get_glyph_dsc(&h2_mfg_factory_font, glyph, '?', 0);
}

static uint16_t blend565(uint16_t fg, uint16_t bg, unsigned alpha) {
  unsigned r =
      (((fg >> 11) & 31u) * alpha + ((bg >> 11) & 31u) * (15u - alpha) + 7u) /
      15u;
  unsigned g =
      (((fg >> 5) & 63u) * alpha + ((bg >> 5) & 63u) * (15u - alpha) + 7u) /
      15u;
  unsigned b = ((fg & 31u) * alpha + (bg & 31u) * (15u - alpha) + 7u) / 15u;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

static int factory_text_width(const char* s, int limit) {
  int width = 0;
  while (*s && width < limit) {
    lv_font_glyph_dsc_t glyph = {0};
    if (factory_glyph(codepoint(&s), &glyph)) width += glyph.adv_w;
    lv_font_glyph_release_draw_data(&glyph);
  }
  return width;
}

static void factory_text(h2_mfg_t* m, const char* s, h2_mfg_ui_rect_t area,
                         uint16_t color, int centered) {
  if (s == NULL || area.width <= 0 || area.height <= 0) return;
  int left = area.x < 0 ? 0 : area.x;
  int right = area.x + area.width;
  if (right > m->display.width) right = m->display.width;
  int bottom = area.y + area.height;
  if (bottom > m->display.height) bottom = m->display.height;
  int available = right - left;
  if (available <= 0) return;
  int width = factory_text_width(s, available);
  if (width > available) width = available;
  int x = centered ? left + (available - width) / 2 : left;
  int y = area.y;
  if (area.height > h2_mfg_factory_font.line_height)
    y += (area.height - h2_mfg_factory_font.line_height) / 2;
  while (*s && x < right) {
    lv_font_glyph_dsc_t glyph = {0};
    if (!factory_glyph(codepoint(&s), &glyph)) {
      lv_font_glyph_release_draw_data(&glyph);
      continue;
    }
    if (x + glyph.adv_w > right) {
      lv_font_glyph_release_draw_data(&glyph);
      break;
    }
    /* The converter emits static, uncompressed A4. Runtime APIs resolve
     * Unicode/metrics; packed alpha is blended into the existing RGB565 frame.
     * No LVGL display, allocator, or second framebuffer is created. */
    /* The pinned converter predates lv_font_t.static_bitmap. Its runtime
     * callback still supports req_raw_bitmap for plain A4 storage. */
    glyph.req_raw_bitmap = 1;
    const uint8_t* bitmap =
        glyph.format == LV_FONT_GLYPH_FORMAT_A4
            ? glyph.resolved_font->get_glyph_bitmap(&glyph, NULL)
            : NULL;
    int top = y + h2_mfg_factory_font.line_height -
              h2_mfg_factory_font.base_line - glyph.box_h - glyph.ofs_y;
    if (bitmap != NULL) {
      for (unsigned row = 0; row < glyph.box_h; ++row) {
        for (unsigned col = 0; col < glyph.box_w; ++col) {
          size_t bit = glyph.stride != 0 ? (size_t)row * glyph.stride * 2u + col
                                         : (size_t)row * glyph.box_w + col;
          unsigned alpha = (bitmap[bit / 2u] >> (bit % 2u ? 0 : 4)) & 15u;
          int px = x + glyph.ofs_x + (int)col, py = top + (int)row;
          if (alpha && px >= left && px < right && py >= area.y && py >= 0 &&
              py < bottom) {
            uint16_t* pixel = &m->frame[(size_t)py * m->display.width + px];
            *pixel = blend565(color, *pixel, alpha);
          }
        }
      }
    }
    x += glyph.adv_w;
    lv_font_glyph_release_draw_data(&glyph);
  }
}

static void factory_center(h2_mfg_t* m, const char* s, unsigned row,
                           uint16_t color) {
  factory_text(m, s, m->profile->ui->rows[row], color, 1);
}

static void factory_rect(h2_mfg_t* m, h2_mfg_ui_rect_t area, uint16_t color) {
  rect(m, area.x, area.y, area.width, area.height, color);
}

static void factory_button(h2_mfg_t* m, h2_mfg_ui_rect_t area, const char* s,
                           int enabled) {
  const h2_mfg_ui_config_t* ui = m->profile->ui;
  factory_rect(m, area, enabled ? ui->accent : ui->disabled);
  factory_text(m, s, area, enabled ? ui->background : ui->muted, 1);
}

static const char* factory_heading(h2_mfg_check_t check) {
  switch (check) {
    case H2_MFG_DISPLAY:
      return "屏幕测试";
    case H2_MFG_BRIGHTNESS:
      return "亮度测试";
    case H2_MFG_TOUCH:
      return "触摸测试";
    case H2_MFG_BUTTONS:
      return "按键测试";
    case H2_MFG_AEC:
      return "音频测试";
    case H2_MFG_BATTERY:
      return "电池测试";
    case H2_MFG_IMU:
      return "运动传感器";
    case H2_MFG_MAGNETOMETERS:
      return "磁力计测试";
    case H2_MFG_VIBRATION:
      return "振动测试";
    case H2_MFG_WIFI:
      return "Wi-Fi 测试";
    case H2_MFG_BLE:
      return "蓝牙扫描";
    case H2_MFG_CAMERA:
      return "摄像头测试";
    default:
      return "硬件测试";
  }
}

static int factory_ble(h2_mfg_t* m, uint64_t now) {
  /* Advertisements arrive on another worker. Copy under its mutex and release
   * before any font work; never retain callback-owned names/addresses. */
  struct {
    h2_pal_ble_addr_t address;
    int rssi;
    char name[32];
  } devices[3];
  unsigned count = 0;
  int error = 0, done = 0;
  if (m->scan_mutex != NULL) {
    int rc = h2_pal_mutex_lock(m->rt->sync, m->scan_mutex);
    if (rc != 0) return rc;
    count = m->scan_count > 32 ? 32 : m->scan_count;
    error = m->scan_error;
    done = m->scan_done;
    unsigned offset =
        count > 3 ? (unsigned)((now / 3000u) % ((count + 2u) / 3u)) * 3u : 0;
    unsigned shown = count - offset;
    if (shown > 3) shown = 3;
    for (unsigned i = 0; i < shown; ++i) {
      devices[i].address = m->scan_devices[offset + i].address;
      devices[i].rssi = m->scan_devices[offset + i].rssi;
      memcpy(devices[i].name, m->scan_devices[offset + i].name, 32);
      devices[i].name[31] = 0;
    }
    int rc_unlock = h2_pal_mutex_unlock(m->rt->sync, m->scan_mutex);
    if (rc_unlock != 0) return rc_unlock;
    count = shown;
  }
  const h2_mfg_ui_config_t* ui = m->profile->ui;
  char line[96];
  if (count == 0)
    factory_center(m, done ? "未发现设备" : "扫描中", 2, ui->text);
  for (unsigned i = 0; i < count; ++i) {
    factory_text(m, devices[i].name[0] ? devices[i].name : "未命名设备",
                 ui->rows[i * 2u], ui->text, 0);
    const uint8_t* a = devices[i].address.value;
    (void)snprintf(line, sizeof(line), "%02X:%02X:%02X:%02X:%02X:%02X  %d dBm",
                   a[0], a[1], a[2], a[3], a[4], a[5], devices[i].rssi);
    factory_text(m, line, ui->rows[i * 2u + 1u], ui->muted, 0);
  }
  if (error) {
    (void)snprintf(line, sizeof(line), "扫描失败 %d", error);
    factory_center(m, line, 6, ui->failed);
  }
  return 0;
}

static int factory_draw(h2_mfg_t* m, uint64_t now) {
  const h2_mfg_ui_config_t* ui = m->profile->ui;
  if (m->display.width <= 0 || m->display.height <= 0 ||
      ui->width != (uint32_t)m->display.width ||
      ui->height != (uint32_t)m->display.height)
    return H2_PAL_ERR_UNSUPPORTED;
  rect(m, 0, 0, m->display.width, m->display.height, ui->background);
  const h2_mfg_step_t* step = m->model.current < m->profile->total
                                  ? &m->profile->steps[m->model.current]
                                  : NULL;
  int failed = m->model.error != 0 ||
               (step && m->model.summary.step_status[m->model.current] ==
                            H2_LOADER_MFG_STEP_FAILED);
  factory_text(m, ui->title, ui->title_area, ui->text, 1);
  factory_text(m, step ? factory_heading(step->check) : "测试完成", ui->heading,
               ui->text, 1);
  unsigned total = m->profile->total;
  if (total > H2_LOADER_MFG_STEP_MAX) total = H2_LOADER_MFG_STEP_MAX;
  if (total && ui->progress.width > 0 && ui->progress.height > 0) {
    int spacing = ui->progress.height * 2 + 2;
    if (spacing > ui->progress.width / (int)total)
      spacing = ui->progress.width / (int)total;
    int size = ui->progress.height < spacing ? ui->progress.height : spacing;
    int left = ui->progress.x + (ui->progress.width - (int)total * spacing) / 2;
    for (unsigned i = 0; i < total; ++i) {
      unsigned status = m->model.summary.step_status[i];
      uint16_t color = status == H2_LOADER_MFG_STEP_PASSED   ? ui->passed
                       : status == H2_LOADER_MFG_STEP_FAILED ? ui->failed
                                                             : ui->muted;
      if (i == m->model.current && m->model.error) color = ui->failed;
      rect(m, left + (int)i * spacing + (spacing - size) / 2,
           ui->progress.y + (ui->progress.height - size) / 2, size, size,
           color);
    }
  }
  char line[160];
  if (!step) {
    factory_center(m, "等待固件刷入", 2, ui->text);
    factory_center(
        m, h2_mfg_model_complete(&m->model) ? "全部通过" : "存在未通过项目", 3,
        h2_mfg_model_complete(&m->model) ? ui->passed : ui->failed);
    factory_button(m, ui->retest, "重新测试", 1);
  } else {
    switch (step->check) {
      case H2_MFG_DISPLAY: {
        static const uint16_t patterns[4] = {RED, GREEN, 0x001f, WHITE};
        factory_rect(m, ui->display_pattern, patterns[m->display_pattern % 4u]);
        (void)snprintf(line, sizeof(line), "点击色块切换 %u / 4",
                       m->display_pattern % 4u + 1u);
        factory_text(m, line, ui->display_note, ui->text, 1);
        break;
      }
      case H2_MFG_BRIGHTNESS:
        for (unsigned i = 0; i < 10; ++i) {
          h2_mfg_ui_rect_t level = ui->brightness_levels;
          int x0 = ((int)i * level.width + 9) / 10;
          int x1 = ((int)(i + 1u) * level.width + 9) / 10;
          unsigned v = (i + 1u) * 255u / 10u;
          uint16_t grey =
              (uint16_t)(((v >> 3) << 11) | ((v >> 2) << 5) | (v >> 3));
          level.x += x0;
          level.width = x1 - x0;
          factory_rect(m, level, grey);
          h2_mfg_ui_rect_t control = ui->brightness_control;
          x0 = ((int)i * control.width + 9) / 10;
          x1 = ((int)(i + 1u) * control.width + 9) / 10;
          control.x += x0;
          control.width = x1 - x0;
          factory_rect(m, control,
                       m->brightness_percent == (i + 1u) * 10u ? ui->accent
                                                               : ui->disabled);
        }
        (void)snprintf(line, sizeof(line), "亮度 %u%%  点击选择 10-100%%",
                       (unsigned)m->brightness_percent);
        factory_text(m, line, ui->brightness_note, ui->text, 1);
        break;
      case H2_MFG_TOUCH:
        for (unsigned i = 0; i < (m->profile->sequential_touch_grid ? 9u : 5u);
             ++i) {
          h2_mfg_ui_rect_t target = ui->touch_targets[i];
          if (!m->profile->sequential_touch_grid) {
            int x, y;
            h2_mfg_touch_target(m, i, &x, &y);
            target = (h2_mfg_ui_rect_t){x - 13, y - 13, 27, 27};
          }
          uint16_t color = m->touch_hits & (1u << i) ? ui->passed
                           : i == m->touch_target    ? ui->accent
                                                     : ui->disabled;
          outline(m, target.x, target.y, target.width, target.height, color);
        }
        if (m->touch_pressed)
          rect(m, m->touch_x - 4, m->touch_y - 4, 8, 8, ui->text);
        break;
      case H2_MFG_BUTTONS: {
        unsigned count = m->profile->button_count;
        if (count > H2_MFG_BUTTON_MAX) count = H2_MFG_BUTTON_MAX;
        for (unsigned i = 0; i < count; ++i) {
          const char* name = m->profile->button_names[i];
          (void)snprintf(line, sizeof(line), "%s %s", name ? name : "",
                         m->button_index > i ? "已完成" : "按下并松开");
          factory_text(m, line, ui->rows[i], ui->text, 1);
        }
        break;
      }
      case H2_MFG_AEC:
        factory_center(m, "请说话并听取回放", 0, ui->text);
        (void)snprintf(line, sizeof(line), "采集播放 %u / %u 帧",
                       m->audio_frames,
                       (unsigned)m->profile->monitor_min_frames);
        factory_center(m, line, 1, ui->text);
        factory_center(
            m, m->model.ready ? "监听就绪  请人工确认" : "等待采集播放就绪", 2,
            m->model.ready ? ui->accent : ui->muted);
        factory_text(m, h2_mfg_aec_phase(m), ui->rows[3], ui->text, 0);
        factory_text(m, m->detail, ui->rows[4], ui->muted, 0);
        break;
      case H2_MFG_BATTERY:
        factory_center(m,
                       m->battery.flags & H2_PAL_BATTERY_PRESENT
                           ? "电池已连接"
                           : "未检测到电池",
                       0, ui->text);
        (void)snprintf(line, sizeof(line), "电压 %ld mV",
                       (long)m->battery.voltage_mv);
        factory_center(
            m,
            m->battery.flags & H2_PAL_BATTERY_HAS_VOLTAGE_MV ? line : "电压 --",
            1, ui->text);
        (void)snprintf(line, sizeof(line), "电流 %+ld mA",
                       (long)m->battery.current_ma);
        factory_center(
            m,
            m->battery.flags & H2_PAL_BATTERY_HAS_CURRENT_MA ? line : "电流 --",
            2, ui->text);
        (void)snprintf(line, sizeof(line), "电量 %u.%02u%%",
                       m->battery.percent_x100 / 100u,
                       m->battery.percent_x100 % 100u);
        factory_center(m,
                       m->battery.flags & H2_PAL_BATTERY_HAS_PERCENT_X100
                           ? line
                           : "电量 --",
                       3, ui->text);
        factory_center(m, m->battery_phase ? "拔除外部电源" : "连接外部电源", 4,
                       ui->accent);
        factory_center(
            m, m->battery_phase ? "确认电池供电正常" : "人工检查充电或充满指示",
            5, ui->text);
        break;
      case H2_MFG_IMU: {
        const h2_pal_vec3_i32_t* a = &m->imu.accel_mg;
        const h2_pal_vec3_i32_t* g = &m->imu.gyro_mdps;
        factory_center(m, "加速度 (mg)", 0, ui->text);
        (void)snprintf(line, sizeof(line), "%ld / %ld / %ld", (long)a->x,
                       (long)a->y, (long)a->z);
        factory_center(m, line, 1, ui->text);
        factory_center(m, "角速度 (mdps)", 2, ui->text);
        (void)snprintf(line, sizeof(line), "%ld / %ld / %ld", (long)g->x,
                       (long)g->y, (long)g->z);
        factory_center(m, line, 3, ui->text);
        factory_center(m,
                       m->accel_responded && m->gyro_responded
                           ? "运动已响应"
                           : "请移动并旋转设备",
                       4, ui->accent);
        break;
      }
      case H2_MFG_MAGNETOMETERS:
        for (unsigned slot = 0; slot < H2_MFG_MAGNETOMETER_MAX; ++slot) {
          unsigned i = ui->magnet_order[slot];
          if (i >= m->profile->magnetometer_count ||
              i >= H2_MFG_MAGNETOMETER_MAX)
            continue;
          factory_text(m, ui->magnet_names[slot], ui->magnet_labels[slot],
                       ui->text, 0);
          h2_mfg_ui_rect_t box = ui->magnet_boxes[slot];
          int error = m->magnet_error[i];
          if (!error && m->magnet_present[i]) factory_rect(m, box, ui->accent);
          outline(m, box.x, box.y, box.width, box.height,
                  error ? ui->failed : ui->accent);
          if (error) {
            (void)snprintf(line, sizeof(line), "读取失败 %d", error);
            factory_text(m, line, ui->magnet_errors[slot], ui->failed, 0);
          }
        }
        break;
      case H2_MFG_VIBRATION:
        factory_rect(m, ui->vibration,
                     m->motor_duty != 0 ? ui->accent : ui->disabled);
        factory_text(m, m->motor_duty != 0 ? "振动中" : "按住振动",
                     ui->vibration, ui->text, 1);
        factory_center(m, "松开停止  请人工确认", 5, ui->text);
        break;
      case H2_MFG_WIFI:
        factory_text(m, m->wifi_status.ssid, ui->rows[0], ui->text, 0);
        if (m->wifi_status.ip_valid && m->wifi_status.ip.ip4) {
          uint8_t ip[4];
          h2_pal_wifi_ip4_to_bytes(m->wifi_status.ip.ip4, ip);
          (void)snprintf(line, sizeof(line), "IPv4 %u.%u.%u.%u", ip[0], ip[1],
                         ip[2], ip[3]);
          factory_center(m, line, 2, ui->text);
        } else
          factory_center(m, "IPv4 等待连接", 2, ui->muted);
        break;
      case H2_MFG_BLE: {
        int rc = factory_ble(m, now);
        if (rc != 0) return rc;
        break;
      }
      case H2_MFG_CAMERA: {
        h2_mfg_ui_rect_t preview = ui->preview;
        if (m->camera_preview && m->preview_width && m->preview_height &&
            preview.x >= 0 && preview.y >= 0 && preview.width > 0 &&
            preview.height > 0 && preview.x < m->display.width &&
            preview.y < m->display.height &&
            m->preview_width <= (uint32_t)preview.width &&
            m->preview_height <= (uint32_t)preview.height &&
            m->preview_width <= (uint32_t)(m->display.width - preview.x) &&
            m->preview_height <= (uint32_t)(m->display.height - preview.y)) {
          for (unsigned row = 0; row < m->preview_height; ++row)
            memcpy(m->frame + ((size_t)preview.y + row) * m->display.width +
                       preview.x,
                   m->camera_preview + (size_t)row * m->preview_width,
                   (size_t)m->preview_width * sizeof(uint16_t));
        } else
          factory_center(m, "等待摄像头画面", 2, ui->muted);
        break;
      }
      default:
        factory_center(m, "请检查硬件", 2, ui->text);
        break;
    }
    if (failed) {
      (void)snprintf(line, sizeof(line), "检测失败 %d", m->model.error);
      factory_center(m, line, 6, ui->failed);
    }
    /* These checks advance on their own fresh input sequence. In failure,
     * retry remains reachable using the shared control geometry. */
    if (failed ||
        (step->check != H2_MFG_TOUCH && step->check != H2_MFG_BUTTONS)) {
      factory_button(m, ui->controls[0], "异常", 1);
      factory_button(m, ui->controls[1], failed ? "重试" : "正常",
                     failed || m->model.ready);
    }
  }
  h2_display_rect_t area = {.width = m->display.width,
                            .height = m->display.height};
  int rc = h2_pal_display_draw_bitmap(
      m->rt->display, &area, m->frame,
      (size_t)m->display.width * sizeof(uint16_t), H2_DISPLAY_PIXEL_RGB565);
  if (rc == 0) rc = h2_pal_display_present(m->rt->display);
  if (rc == 0 && step && step->check == H2_MFG_DISPLAY && !failed)
    m->display_patterns_seen |= 1u << (m->display_pattern % 4u);
  return rc;
}

int h2_mfg_ui_draw(h2_mfg_t* m, uint64_t now) {
  if (m->frame == NULL || !m->display_open) return 0;
  if (m->profile->ui != NULL) return factory_draw(m, now);
  int w = m->display.width, h = m->display.height;
  int scale = w >= 320 ? 2 : 1;
  rect(m, 0, 0, w, h, BG);
  const h2_mfg_step_t* step = m->model.current < m->profile->total
                                  ? &m->profile->steps[m->model.current]
                                  : NULL;
  int failed =
      m->model.error != 0 ||
      (step != NULL && m->model.summary.step_status[m->model.current] ==
                           H2_LOADER_MFG_STEP_FAILED);
  if (step != NULL && step->check == H2_MFG_DISPLAY && !failed) {
    if (m->profile->manual_display_patterns) {
      static const uint16_t patterns[] = {RED, GREEN, 0x001f, WHITE};
      m->display_pattern = (unsigned)((now - m->started_ms) / 600u) % 4u;
      rect(m, 0, 0, w, h, patterns[m->display_pattern]);
    } else {
      rect(m, 0, 0, w / 2, h / 2, RED);
      rect(m, w / 2, 0, w - w / 2, h / 2, GREEN);
      rect(m, 0, h / 2, w / 2, h - h / 2, 0x001f);
      rect(m, w / 2, h / 2, w - w / 2, h - h / 2, WHITE);
    }
  }
  rect(m, w * 15 / 100, h * 5 / 100, w * 70 / 100, h * 14 / 100, BG);
  char line[96];
  (void)snprintf(
      line, sizeof(line), "MFG %u OF %u",
      (unsigned)(step == NULL ? m->profile->total : m->model.current + 1u),
      (unsigned)m->profile->total);
  center(m, line, h * 7 / 100, scale, WHITE);
  center(m,
         step == NULL ? (h2_mfg_model_complete(&m->model) ? "ALL PASSED"
                                                          : "REVIEW REQUIRED")
                      : step->name,
         h * 13 / 100, scale, failed ? RED : WHITE);
  if (step != NULL && step->check == H2_MFG_TOUCH) {
    for (unsigned i = 0; i < 5; ++i) {
      int x, y;
      h2_mfg_touch_target(m, i, &x, &y);
      uint16_t color = m->touch_hits & (1u << i) ? GREEN : WHITE;
      rect(m, x - 13, y - 1, 27, 3, color);
      rect(m, x - 1, y - 13, 3, 27, color);
    }
    if (m->touch_pressed) disk(m, m->touch_x, m->touch_y, 5, BLUE);
  } else if (step != NULL && step->check == H2_MFG_IMU) {
    int radius = w / 7;
    disk(m, w / 2, h / 2, radius + 2, GREY);
    disk(m, w / 2, h / 2, radius, BG);
    disk(m, w / 2 + clamp(imu_axis(m, m->profile->screen_x_axis), radius - 10),
         h / 2 + clamp(imu_axis(m, m->profile->screen_y_axis), radius - 10), 8,
         GREEN);
  } else if (step != NULL && step->check == H2_MFG_BUTTONS) {
    unsigned first =
        m->profile->button_count > 4 ? (unsigned)((now / 4000u) % 2u) * 4u : 0u;
    for (unsigned i = first; i < m->profile->button_count && i < first + 4;
         ++i) {
      (void)snprintf(line, sizeof(line), "%s DOWN %u UP %u",
                     m->profile->button_names[i], m->downs[i], m->ups[i]);
      center(m, line, h * (30 + (int)(i - first) * 12) / 100, scale,
             m->pressed[i] ? GREEN : WHITE);
    }
    if (m->profile->has_power_button)
      center(m, "POWER BOARD OFF ON", h * 45 / 100, scale, WHITE);
    if (m->profile->has_reset_button)
      center(m, "RESET RESTARTS BOARD", h * 57 / 100, scale, WHITE);
    if (m->profile->has_reset_button || m->profile->has_power_button) {
      center(m, "THEN BOOT THREE CYCLES", h * 69 / 100, scale, WHITE);
    }
  } else if (step != NULL && step->check == H2_MFG_CAMERA) {
    if (m->camera_preview != NULL) {
      int left = (w - (int)m->preview_width) / 2, top = h * 22 / 100;
      for (unsigned row = 0; row < m->preview_height; ++row)
        for (unsigned col = 0; col < m->preview_width; ++col)
          rect(m, left + (int)col, top + (int)row, 1, 1,
               m->camera_preview[(size_t)row * m->preview_width + col]);
    }
    center(m, "MOVE AN OBJECT", h * 61 / 100, scale, WHITE);
    center(m, "CONFIRM LIVE IMAGE", h * 69 / 100, scale, WHITE);
  } else if (step != NULL && step->check == H2_MFG_MAGNETOMETERS) {
    center(m, "ROTATE THE BOARD", h * 30 / 100, scale, WHITE);
    for (unsigned i = 0; i < m->profile->magnetometer_count; ++i) {
      const h2_pal_vec3_i32_t* v = &m->magnetometers[i].mag_mgauss;
      (void)snprintf(line, sizeof(line), "MAG %u  %ld %ld %ld", i + 1,
                     (long)v->x, (long)v->y, (long)v->z);
      center(m, line, h * (44 + (int)i * 14) / 100, scale, WHITE);
    }
    center(m, "BOTH MUST CHANGE", h * 69 / 100, scale, WHITE);
  } else if (step != NULL && step->check == H2_MFG_BATTERY) {
    (void)snprintf(line, sizeof(line), "VOLTAGE %ld MV",
                   (long)m->battery.voltage_mv);
    center(m, line, h * 36 / 100, scale, WHITE);
    (void)snprintf(line, sizeof(line), "CHARGE %u PERCENT",
                   (unsigned)m->battery.percent_x100 / 100);
    center(m, line, h * 50 / 100, scale, WHITE);
    center(m, "CHECK BATTERY PRESENT", h * 66 / 100, scale, WHITE);
  } else if (step != NULL && step->check == H2_MFG_VIBRATION) {
    center(m, "FEEL THE VIBRATION", h * 34 / 100, scale, WHITE);
    center(m, "200 MS EVERY 2 SEC", h * 48 / 100, scale, WHITE);
    (void)snprintf(line, sizeof(line), "PULSES %u", m->motor_pulses);
    center(m, line, h * 64 / 100, scale, WHITE);
  } else if (step != NULL && step->check == H2_MFG_MIC) {
    center(m, "SPEAK NEAR MIC", h * 34 / 100, scale, WHITE);
    unsigned bars = 0;
    for (unsigned threshold = 32; threshold <= m->mic_level && bars < 10;
         threshold *= 2u)
      ++bars;
    for (unsigned i = 0; i < 10; ++i)
      rect(m, w * (20 + (int)i * 6) / 100, h * 47 / 100, w * 5 / 100,
           h * 9 / 100,
           i >= bars ? GREY
           : i < 8   ? GREEN
           : i < 9   ? 0xffe0
                     : RED);
    (void)snprintf(line, sizeof(line), "LEVEL %u", m->mic_level);
    center(m, line, h * 62 / 100, scale, WHITE);
    (void)snprintf(line, sizeof(line), "FRAMES %u", m->audio_frames);
    center(m, line, h * 69 / 100, scale, WHITE);
  } else if (step != NULL && step->check == H2_MFG_SPEAKER) {
    center(m, "LISTEN TO TONE", h * 34 / 100, scale, WHITE);
    center(m, "440 HZ", h * 46 / 100, scale, WHITE);
    center(m, "EVERY 2 SECONDS", h * 58 / 100, scale, WHITE);
    center(m, "CHECK DISTORTION", h * 69 / 100, scale, WHITE);
  } else if (step != NULL && step->check == H2_MFG_AEC) {
    center(m, h2_mfg_aec_phase(m), h * 32 / 100, scale, WHITE);
    int echo_db = h2_mfg_aec_echo_db(m);
    if (echo_db >= 0)
      (void)snprintf(line, sizeof(line), "ECHO REDUCTION %d DB", echo_db);
    else
      (void)snprintf(line, sizeof(line), "LISTEN FOR ECHO");
    center(m, line, h * 44 / 100, scale, echo_db >= 0 ? GREEN : WHITE);
    (void)snprintf(line, sizeof(line), "MIC LEVEL %u", m->mic_level);
    center(m, line, h * 56 / 100, scale, WHITE);
    center(m, m->model.ready ? "CHECK ECHO AND VOICE" : "FOLLOW THE PROMPT",
           h * 68 / 100, scale, WHITE);
  } else if (step != NULL && step->check != H2_MFG_DISPLAY) {
    const char* prompt = step->check == H2_MFG_WIFI  ? "CONNECT WIFI BY SERIAL"
                         : step->check == H2_MFG_BLE ? "CONNECT A BOARD PEER"
                                                     : "CHECK HARDWARE";
    center(m, prompt, h * 40 / 100, scale, WHITE);
  }
  if (step != NULL) {
    rect(m, w * 10 / 100, h * 83 / 100, w * 40 / 100, h * 10 / 100,
         failed           ? BLUE
         : m->model.ready ? GREEN
                          : GREY);
    rect(m, w / 2, h * 83 / 100, w * 40 / 100, h * 10 / 100, RED);
    text(m,
         failed           ? "RETRY"
         : m->model.ready ? "PASS"
                          : "WAIT",
         w * 22 / 100, h * 86 / 100, scale, WHITE);
    text(m, "FAIL", w * 62 / 100, h * 86 / 100, scale, WHITE);
    if (failed)
      (void)snprintf(line, sizeof(line), "ERROR %d  SERIAL FOR DETAILS",
                     m->model.error);
    else
      (void)snprintf(line, sizeof(line), "%s CONFIRMS  TICKET %08x",
                     m->profile->button_count != 0 ? m->profile->button_names[0]
                                                   : "SERIAL",
                     (unsigned)m->model.ticket);
    center(m, line, h * 79 / 100, 1, failed ? RED : WHITE);
  }
  h2_display_rect_t area = {.width = w, .height = h};
  int rc = h2_pal_display_draw_bitmap(m->rt->display, &area, m->frame,
                                      (size_t)w * sizeof(*m->frame),
                                      H2_DISPLAY_PIXEL_RGB565);
  if (rc == 0) rc = h2_pal_display_present(m->rt->display);
  if (rc == 0 && step && step->check == H2_MFG_DISPLAY && !failed &&
      m->profile->manual_display_patterns)
    m->display_patterns_seen |= 1u << m->display_pattern;
  return rc;
}
