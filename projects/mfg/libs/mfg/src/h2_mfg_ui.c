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
  *x = m->display.width * xy[i][0] / 100;
  *y = m->display.height * xy[i][1] / 100;
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

int h2_mfg_ui_draw(h2_mfg_t* m, uint64_t now) {
  if (m->frame == NULL || !m->display_open) return 0;
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
    rect(m, 0, 0, w / 2, h / 2, RED);
    rect(m, w / 2, 0, w - w / 2, h / 2, GREEN);
    rect(m, 0, h / 2, w / 2, h - h / 2, 0x001f);
    rect(m, w / 2, h / 2, w - w / 2, h - h / 2, WHITE);
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
      int left = (w - H2_MFG_PREVIEW_WIDTH) / 2, top = h * 22 / 100;
      for (int row = 0; row < H2_MFG_PREVIEW_HEIGHT; ++row)
        for (int col = 0; col < H2_MFG_PREVIEW_WIDTH; ++col)
          rect(m, left + col, top + row, 1, 1,
               m->camera_preview[row * H2_MFG_PREVIEW_WIDTH + col]);
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
  return rc == 0 ? h2_pal_display_present(m->rt->display) : rc;
}
