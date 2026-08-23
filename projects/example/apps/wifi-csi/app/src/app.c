#include "h2_smoke_wifi_csi.h"

#include "h2/pal/hal/h2_pal_display.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/net/h2_pal_net.h"
#include "h2/pal/os/h2_pal_sync.h"
#include "h2/pal/os/h2_pal_time.h"
#include "h2/pal/hal/h2_pal_wifi.h"
#include "h2/pal/hal/h2_pal_wifi_csi.h"
#include "h2/pal/hal/h2_pal_wifi_settings.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define H2_SMOKE_WIFI_CSI_SAMPLE_LIMIT 96u
#define H2_SMOKE_WIFI_CSI_RENDER_ROWS 16u
#define H2_SMOKE_WIFI_CSI_DEFAULT_WIFI_TIMEOUT_MS 10000u
#define H2_SMOKE_WIFI_CSI_DEFAULT_RETRY_MS 5000u
#define H2_SMOKE_WIFI_CSI_DEFAULT_REFRESH_MS 250u
#define H2_SMOKE_WIFI_CSI_NO_FRAME_MS 5000u
#define H2_SMOKE_WIFI_CSI_PROBE_INTERVAL_MS 250u
#define H2_SMOKE_WIFI_CSI_PROBE_TIMEOUT_MS 100u
#define H2_SMOKE_WIFI_CSI_TEXT_LIMIT 24u

typedef enum h2_smoke_wifi_csi_status {
  H2_SMOKE_WIFI_CSI_STATUS_CONNECTING = 0,
  H2_SMOKE_WIFI_CSI_STATUS_CAPTURING,
  H2_SMOKE_WIFI_CSI_STATUS_BACKOFF,
  H2_SMOKE_WIFI_CSI_STATUS_NO_SAVED_WIFI,
  H2_SMOKE_WIFI_CSI_STATUS_UNSUPPORTED,
  H2_SMOKE_WIFI_CSI_STATUS_NO_FRAMES,
  H2_SMOKE_WIFI_CSI_STATUS_INVALID_FRAME,
  H2_SMOKE_WIFI_CSI_STATUS_PROVIDER_ERROR,
} h2_smoke_wifi_csi_status_t;

typedef enum h2_smoke_wifi_csi_error_stage {
  H2_SMOKE_WIFI_CSI_ERROR_NONE = 0,
  H2_SMOKE_WIFI_CSI_ERROR_CAPABILITIES,
  H2_SMOKE_WIFI_CSI_ERROR_START,
} h2_smoke_wifi_csi_error_stage_t;

typedef struct h2_smoke_wifi_csi_state {
  h2_runtime_t *runtime;
  h2_pal_mutex_t *mutex;
  h2_smoke_wifi_csi_status_t status;
  h2_smoke_wifi_csi_error_stage_t error_stage;
  int last_error;
  h2_pal_wifi_csi_provider_t provider;
  h2_pal_wifi_csi_phy_t phy;
  uint8_t channel;
  uint8_t bandwidth_mhz;
  uint8_t mcs;
  int8_t rssi_dbm;
  uint32_t frame_count;
  uint32_t invalid_frame_count;
  uint64_t last_frame_ms;
  uint64_t next_attempt_ms;
  size_t sample_count;
  h2_pal_wifi_csi_sample_t samples[H2_SMOKE_WIFI_CSI_SAMPLE_LIMIT];
  int csi_started;
} h2_smoke_wifi_csi_state_t;

typedef struct h2_smoke_wifi_csi_dashboard {
  h2_smoke_wifi_csi_status_t status;
  h2_smoke_wifi_csi_error_stage_t error_stage;
  int last_error;
  h2_pal_wifi_csi_provider_t provider;
  uint8_t channel;
  uint8_t bandwidth_mhz;
  uint8_t mcs;
  int8_t rssi_dbm;
  uint32_t frame_count;
  uint32_t invalid_frame_count;
  size_t sample_count;
  h2_pal_wifi_csi_sample_t samples[H2_SMOKE_WIFI_CSI_SAMPLE_LIMIT];
} h2_smoke_wifi_csi_dashboard_t;

static const uint8_t s_glyphs[38][5] = {
    {0x1eu, 0x05u, 0x05u, 0x1eu, 0x00u}, {0x1fu, 0x15u, 0x15u, 0x0au, 0x00u},
    {0x0eu, 0x11u, 0x11u, 0x0au, 0x00u}, {0x1fu, 0x11u, 0x11u, 0x0eu, 0x00u},
    {0x1fu, 0x15u, 0x15u, 0x11u, 0x00u}, {0x1fu, 0x05u, 0x05u, 0x01u, 0x00u},
    {0x0eu, 0x11u, 0x15u, 0x1du, 0x00u}, {0x1fu, 0x04u, 0x04u, 0x1fu, 0x00u},
    {0x11u, 0x1fu, 0x11u, 0x00u, 0x00u}, {0x08u, 0x10u, 0x10u, 0x0fu, 0x00u},
    {0x1fu, 0x04u, 0x0au, 0x11u, 0x00u}, {0x1fu, 0x10u, 0x10u, 0x10u, 0x00u},
    {0x1fu, 0x02u, 0x04u, 0x02u, 0x1fu}, {0x1fu, 0x02u, 0x04u, 0x1fu, 0x00u},
    {0x0eu, 0x11u, 0x11u, 0x0eu, 0x00u}, {0x1fu, 0x05u, 0x05u, 0x02u, 0x00u},
    {0x0eu, 0x11u, 0x19u, 0x1eu, 0x00u}, {0x1fu, 0x05u, 0x0du, 0x12u, 0x00u},
    {0x12u, 0x15u, 0x15u, 0x09u, 0x00u}, {0x01u, 0x1fu, 0x01u, 0x00u, 0x00u},
    {0x0fu, 0x10u, 0x10u, 0x0fu, 0x00u}, {0x07u, 0x08u, 0x10u, 0x08u, 0x07u},
    {0x1fu, 0x08u, 0x04u, 0x08u, 0x1fu}, {0x1bu, 0x04u, 0x04u, 0x1bu, 0x00u},
    {0x03u, 0x04u, 0x18u, 0x04u, 0x03u}, {0x19u, 0x15u, 0x13u, 0x00u, 0x00u},
    {0x0eu, 0x11u, 0x11u, 0x0eu, 0x00u}, {0x00u, 0x12u, 0x1fu, 0x10u, 0x00u},
    {0x19u, 0x15u, 0x15u, 0x12u, 0x00u}, {0x11u, 0x15u, 0x15u, 0x0au, 0x00u},
    {0x07u, 0x04u, 0x04u, 0x1fu, 0x00u}, {0x17u, 0x15u, 0x15u, 0x09u, 0x00u},
    {0x0eu, 0x15u, 0x15u, 0x09u, 0x00u}, {0x01u, 0x01u, 0x1du, 0x03u, 0x00u},
    {0x0au, 0x15u, 0x15u, 0x0au, 0x00u}, {0x12u, 0x15u, 0x15u, 0x0eu, 0x00u},
    {0x00u, 0x00u, 0x00u, 0x00u, 0x00u}, {0x00u, 0x0au, 0x00u, 0x00u, 0x00u},
};

static uint16_t
h2_smoke_wifi_csi_status_color(h2_smoke_wifi_csi_status_t status) {
  if (status == H2_SMOKE_WIFI_CSI_STATUS_CAPTURING) {
    return 0x07e0u;
  }
  if (status == H2_SMOKE_WIFI_CSI_STATUS_CONNECTING ||
      status == H2_SMOKE_WIFI_CSI_STATUS_NO_FRAMES) {
    return 0xffe0u;
  }
  return 0xf800u;
}

static const char *
h2_smoke_wifi_csi_status_text(h2_smoke_wifi_csi_status_t status) {
  switch (status) {
  case H2_SMOKE_WIFI_CSI_STATUS_CAPTURING:
    return "CAPTURING";
  case H2_SMOKE_WIFI_CSI_STATUS_BACKOFF:
    return "RETRYING";
  case H2_SMOKE_WIFI_CSI_STATUS_NO_SAVED_WIFI:
    return "NO SAVED WIFI";
  case H2_SMOKE_WIFI_CSI_STATUS_UNSUPPORTED:
    return "CSI UNSUPPORTED";
  case H2_SMOKE_WIFI_CSI_STATUS_NO_FRAMES:
    return "NO FRAMES";
  case H2_SMOKE_WIFI_CSI_STATUS_INVALID_FRAME:
    return "INVALID FRAME";
  case H2_SMOKE_WIFI_CSI_STATUS_PROVIDER_ERROR:
    return "PROVIDER ERROR";
  case H2_SMOKE_WIFI_CSI_STATUS_CONNECTING:
  default:
    return "CONNECTING";
  }
}

static void h2_smoke_wifi_csi_log(h2_smoke_wifi_csi_state_t *state,
                                  const char *stage, int rc, int detail) {
  char message[96];

  if (state == NULL || state->runtime == NULL || state->runtime->log == NULL)
    return;
  (void)snprintf(message, sizeof(message),
                 "H2_WIFI_CSI stage=%s rc=%d detail=%d", stage, rc, detail);
  (void)h2_pal_log_write(state->runtime->log, H2_PAL_LOG_INFO, "wifi-csi",
                         message);
}

static int h2_smoke_wifi_csi_glyph_index(char value) {
  if (value >= 'A' && value <= 'Z')
    return value - 'A';
  if (value >= '0' && value <= '9')
    return 26 + value - '0';
  if (value == '-')
    return 37;
  return 36;
}

static int h2_smoke_wifi_csi_text_pixel(const char *text, int x, int y,
                                        int scale, int px, int py) {
  if (px < x || py < y || scale <= 0)
    return 0;
  int char_index = (px - x) / (6 * scale);
  if (char_index < 0 || char_index >= (int)H2_SMOKE_WIFI_CSI_TEXT_LIMIT)
    return 0;
  char value = text[char_index];
  if (value == '\0')
    return 0;
  int gx = ((px - x) / scale) % 6;
  int gy = (py - y) / scale;
  if (gx >= 5 || gy >= 7)
    return 0;
  return (s_glyphs[h2_smoke_wifi_csi_glyph_index(value)][gx] & (1u << gy)) !=
         0u;
}

static size_t h2_smoke_wifi_csi_u32_text(uint32_t value, char out[12]) {
  char reverse[10];
  size_t count = 0u;
  do {
    reverse[count++] = (char)('0' + value % 10u);
    value /= 10u;
  } while (value != 0u && count < sizeof(reverse));
  for (size_t index = 0u; index < count; ++index)
    out[index] = reverse[count - index - 1u];
  out[count] = '\0';
  return count;
}

static void h2_smoke_wifi_csi_metric_text(const char *label, int value,
                                          char out[24]) {
  size_t at = 0u;
  uint32_t magnitude =
      value < 0 ? (uint32_t)(-(int64_t)value) : (uint32_t)value;
  while (label[at] != '\0' && at < 16u) {
    out[at] = label[at];
    at++;
  }
  out[at++] = '-';
  if (value < 0) {
    out[at++] = '-';
  }
  char number[12];
  size_t count = h2_smoke_wifi_csi_u32_text(magnitude, number);
  for (size_t index = 0u; index < count && at + 1u < 24u; ++index)
    out[at++] = number[index];
  out[at] = '\0';
}

static int h2_smoke_wifi_csi_amplitude(const h2_pal_wifi_csi_sample_t *sample) {
  int real = sample->real;
  int imag = sample->imag;
  int amplitude = (real < 0 ? -real : real) + (imag < 0 ? -imag : imag);
  return amplitude > 255 ? 255 : amplitude;
}

static int
h2_smoke_wifi_csi_wave_y(const h2_smoke_wifi_csi_dashboard_t *dashboard,
                         int graph_y, int graph_height, int graph_width,
                         int x) {
  if (dashboard->sample_count == 0u || graph_width <= 1)
    return graph_y + graph_height / 2;
  if (dashboard->sample_count == 1u)
    return graph_y + graph_height - 1 -
           h2_smoke_wifi_csi_amplitude(&dashboard->samples[0]) *
               (graph_height - 1) / 255;

  size_t denominator = (size_t)graph_width - 1u;
  size_t position = (size_t)x * (dashboard->sample_count - 1u);
  size_t left = position / denominator;
  size_t remainder = position % denominator;
  size_t right = left + 1u < dashboard->sample_count ? left + 1u : left;
  int left_amplitude = h2_smoke_wifi_csi_amplitude(&dashboard->samples[left]);
  int right_amplitude = h2_smoke_wifi_csi_amplitude(&dashboard->samples[right]);
  int amplitude = (int)(((size_t)left_amplitude * (denominator - remainder) +
                         (size_t)right_amplitude * remainder) /
                        denominator);
  return graph_y + graph_height - 1 - amplitude * (graph_height - 1) / 255;
}

static uint16_t
h2_smoke_wifi_csi_pixel(const h2_smoke_wifi_csi_dashboard_t *dashboard,
                        int width, int height, int x, int y) {
  const uint16_t background = 0x1082u;
  const uint16_t panel = 0x18e3u;
  const uint16_t grid = 0x2945u;
  const uint16_t white = 0xffffu;
  int scale = width >= 600 ? 3 : 2;
  int margin = width / 20;
  int graph_x = margin;
  int graph_y = height / 3;
  int graph_width = width - margin * 2;
  int graph_height = height / 3;
  char primary[24];
  char rssi[24];
  char frames[24];
  char samples[24];
  char invalid[24];

  uint16_t color = background;
  if (y < height / 7)
    color = 0x001fu;
  if (x >= graph_x && x < graph_x + graph_width && y >= graph_y &&
      y < graph_y + graph_height) {
    color = panel;
    if ((x - graph_x) % (graph_width / 8 + 1) == 0 ||
        (y - graph_y) % (graph_height / 4 + 1) == 0)
      color = grid;
    if (dashboard->sample_count > 0u) {
      int wave_y = h2_smoke_wifi_csi_wave_y(dashboard, graph_y, graph_height,
                                            graph_width, x - graph_x);
      int previous_wave_y = wave_y;
      if (x > graph_x)
        previous_wave_y = h2_smoke_wifi_csi_wave_y(
            dashboard, graph_y, graph_height, graph_width, x - graph_x - 1);
      int wave_top = wave_y < previous_wave_y ? wave_y : previous_wave_y;
      int wave_bottom = wave_y > previous_wave_y ? wave_y : previous_wave_y;
      if (y >= wave_top - 1 && y <= wave_bottom + 1)
        color = 0x07ffu;
    }
  }

  if (dashboard->error_stage == H2_SMOKE_WIFI_CSI_ERROR_CAPABILITIES) {
    h2_smoke_wifi_csi_metric_text("CAP", dashboard->last_error, primary);
  } else if (dashboard->error_stage == H2_SMOKE_WIFI_CSI_ERROR_START) {
    h2_smoke_wifi_csi_metric_text("START", dashboard->last_error, primary);
  } else {
    h2_smoke_wifi_csi_metric_text("CH", dashboard->channel, primary);
  }
  h2_smoke_wifi_csi_metric_text("RSSI", dashboard->rssi_dbm, rssi);
  h2_smoke_wifi_csi_metric_text("FRAMES", (int)dashboard->frame_count, frames);
  h2_smoke_wifi_csi_metric_text("SAMPLES", (int)dashboard->sample_count,
                                samples);
  h2_smoke_wifi_csi_metric_text(
      "INVALID", (int)dashboard->invalid_frame_count, invalid);
  if (h2_smoke_wifi_csi_text_pixel("CSI SMOKE", margin, height / 28, scale, x,
                                   y) ||
      h2_smoke_wifi_csi_text_pixel(
          h2_smoke_wifi_csi_status_text(dashboard->status), margin,
          height / 7 + 8, scale, x, y)) {
    return y < height / 7 ? white
                          : h2_smoke_wifi_csi_status_color(dashboard->status);
  }
  if (h2_smoke_wifi_csi_text_pixel(
          primary, margin, graph_y + graph_height + height / 16, scale, x, y) ||
      h2_smoke_wifi_csi_text_pixel(
          rssi, margin, graph_y + graph_height + height / 16 + 9 * scale, scale,
          x, y) ||
      h2_smoke_wifi_csi_text_pixel(frames, width / 2,
                                   graph_y + graph_height + height / 16, scale,
                                   x, y) ||
      h2_smoke_wifi_csi_text_pixel(
          samples, width / 2, graph_y + graph_height + height / 16 + 9 * scale,
          scale, x, y) ||
      h2_smoke_wifi_csi_text_pixel(
          invalid, width / 2,
          graph_y + graph_height + height / 16 + 18 * scale, scale, x, y)) {
    return white;
  }
  return color;
}

static int
h2_smoke_wifi_csi_snapshot(h2_smoke_wifi_csi_state_t *state,
                           h2_smoke_wifi_csi_dashboard_t *out_dashboard) {
  if (h2_pal_mutex_lock(state->runtime->sync, state->mutex) != H2_PAL_OK)
    return H2_PAL_ERR_IO;
  out_dashboard->status = state->status;
  out_dashboard->error_stage = state->error_stage;
  out_dashboard->last_error = state->last_error;
  out_dashboard->provider = state->provider;
  out_dashboard->channel = state->channel;
  out_dashboard->bandwidth_mhz = state->bandwidth_mhz;
  out_dashboard->mcs = state->mcs;
  out_dashboard->rssi_dbm = state->rssi_dbm;
  out_dashboard->frame_count = state->frame_count;
  out_dashboard->invalid_frame_count = state->invalid_frame_count;
  out_dashboard->sample_count = state->sample_count;
  memcpy(out_dashboard->samples, state->samples,
         state->sample_count * sizeof(state->samples[0]));
  (void)h2_pal_mutex_unlock(state->runtime->sync, state->mutex);
  return H2_PAL_OK;
}

static int
h2_smoke_wifi_csi_dashboard_equal(const h2_smoke_wifi_csi_dashboard_t *left,
                                  const h2_smoke_wifi_csi_dashboard_t *right) {
  if (left->status != right->status ||
      left->error_stage != right->error_stage ||
      left->last_error != right->last_error ||
      left->provider != right->provider || left->channel != right->channel ||
      left->bandwidth_mhz != right->bandwidth_mhz || left->mcs != right->mcs ||
      left->rssi_dbm != right->rssi_dbm ||
      left->frame_count != right->frame_count ||
      left->invalid_frame_count != right->invalid_frame_count ||
      left->sample_count != right->sample_count)
    return 0;
  return left->sample_count == 0u ||
         memcmp(left->samples, right->samples,
                left->sample_count * sizeof(left->samples[0])) == 0;
}

static int h2_smoke_wifi_csi_get_progress(h2_smoke_wifi_csi_state_t *state,
                                          int *out_csi_started,
                                          uint64_t *out_last_frame_ms,
                                          uint64_t *out_next_attempt_ms) {
  if (h2_pal_mutex_lock(state->runtime->sync, state->mutex) != H2_PAL_OK)
    return H2_PAL_ERR_IO;
  *out_csi_started = state->csi_started;
  *out_last_frame_ms = state->last_frame_ms;
  *out_next_attempt_ms = state->next_attempt_ms;
  (void)h2_pal_mutex_unlock(state->runtime->sync, state->mutex);
  return H2_PAL_OK;
}

static int
h2_smoke_wifi_csi_render(const h2_pal_display_api_t *display,
                         const h2_smoke_wifi_csi_dashboard_t *dashboard,
                         const h2_display_info_t *info, uint16_t *rows) {
  int rc;
  for (int y = 0; y < info->height; y += (int)H2_SMOKE_WIFI_CSI_RENDER_ROWS) {
    int row_count = info->height - y;
    if (row_count > (int)H2_SMOKE_WIFI_CSI_RENDER_ROWS)
      row_count = H2_SMOKE_WIFI_CSI_RENDER_ROWS;
    for (int yy = 0; yy < row_count; ++yy) {
      for (int x = 0; x < info->width; ++x) {
        rows[(size_t)yy * (size_t)info->width + (size_t)x] =
            h2_smoke_wifi_csi_pixel(dashboard, info->width, info->height, x,
                                    y + yy);
      }
    }
    h2_display_rect_t rect = {
        .x = 0, .y = y, .width = info->width, .height = row_count};
    rc = h2_pal_display_draw_bitmap(display, &rect, rows,
                                    (size_t)info->width * sizeof(uint16_t),
                                    H2_DISPLAY_PIXEL_RGB565);
    if (rc != H2_DISPLAY_OK)
      return rc;
  }
  return h2_pal_display_present(display);
}

static void
h2_smoke_wifi_csi_frame_received(void *user,
                                 const h2_pal_wifi_csi_frame_t *frame) {
  h2_smoke_wifi_csi_state_t *state = user;
  if (state == NULL || frame == NULL ||
      h2_pal_mutex_try_lock(state->runtime->sync, state->mutex) != H2_PAL_OK)
    return;
  state->error_stage = H2_SMOKE_WIFI_CSI_ERROR_NONE;
  state->last_error = H2_PAL_OK;
  if (frame->samples == NULL || frame->sample_count == 0u) {
    state->invalid_frame_count++;
    if (state->frame_count == 0u)
      state->status = H2_SMOKE_WIFI_CSI_STATUS_INVALID_FRAME;
  } else {
    state->provider = frame->provider;
    state->phy = frame->phy;
    state->channel = frame->channel;
    state->bandwidth_mhz = frame->bandwidth_mhz;
    state->mcs = frame->mcs;
    state->rssi_dbm = frame->rssi_dbm;
    state->sample_count = frame->sample_count > H2_SMOKE_WIFI_CSI_SAMPLE_LIMIT
                              ? H2_SMOKE_WIFI_CSI_SAMPLE_LIMIT
                              : frame->sample_count;
    memcpy(state->samples, frame->samples,
           state->sample_count * sizeof(state->samples[0]));
    state->frame_count++;
    state->status = H2_SMOKE_WIFI_CSI_STATUS_CAPTURING;
    (void)h2_pal_time_get_monotonic_ms(state->runtime->time,
                                       &state->last_frame_ms);
  }
  (void)h2_pal_mutex_unlock(state->runtime->sync, state->mutex);
}

static void h2_smoke_wifi_csi_set_status(h2_smoke_wifi_csi_state_t *state,
                                         h2_smoke_wifi_csi_status_t status,
                                         uint64_t next_attempt_ms) {
  if (h2_pal_mutex_lock(state->runtime->sync, state->mutex) != H2_PAL_OK)
    return;
  state->status = status;
  state->error_stage = H2_SMOKE_WIFI_CSI_ERROR_NONE;
  state->last_error = H2_PAL_OK;
  state->next_attempt_ms = next_attempt_ms;
  (void)h2_pal_mutex_unlock(state->runtime->sync, state->mutex);
}

static void h2_smoke_wifi_csi_set_error(h2_smoke_wifi_csi_state_t *state,
                                        h2_smoke_wifi_csi_status_t status,
                                        h2_smoke_wifi_csi_error_stage_t stage,
                                        int error, uint64_t next_attempt_ms) {
  if (h2_pal_mutex_lock(state->runtime->sync, state->mutex) != H2_PAL_OK)
    return;
  state->status = status;
  state->error_stage = stage;
  state->last_error = error;
  state->next_attempt_ms = next_attempt_ms;
  (void)h2_pal_mutex_unlock(state->runtime->sync, state->mutex);
}

static void h2_smoke_wifi_csi_stop_for_retry(h2_smoke_wifi_csi_state_t *state,
                                             uint64_t next_attempt_ms) {
  (void)h2_pal_wifi_csi_stop(state->runtime->wifi_csi);
  if (h2_pal_mutex_lock(state->runtime->sync, state->mutex) != H2_PAL_OK)
    return;
  state->csi_started = 0;
  state->status = H2_SMOKE_WIFI_CSI_STATUS_BACKOFF;
  state->error_stage = H2_SMOKE_WIFI_CSI_ERROR_NONE;
  state->last_error = H2_PAL_OK;
  state->next_attempt_ms = next_attempt_ms;
  (void)h2_pal_mutex_unlock(state->runtime->sync, state->mutex);
}

static void h2_smoke_wifi_csi_try_start(h2_smoke_wifi_csi_state_t *state,
                                        uint64_t now_ms,
                                        uint32_t wifi_timeout_ms,
                                        uint32_t retry_ms) {
  h2_pal_wifi_sta_config_t saved;
  h2_pal_wifi_sta_status_t wifi_status;
  h2_pal_wifi_csi_config_t csi_config;
  h2_pal_wifi_csi_capabilities_t capabilities;
  uint32_t frame_count_before_start;
  int rc;

  memset(&wifi_status, 0, sizeof(wifi_status));
  rc = h2_pal_wifi_sta_get_status(state->runtime->wifi_sta, &wifi_status);
  h2_smoke_wifi_csi_log(state, "wifi_status", rc, (int)wifi_status.state);
  if (rc != H2_PAL_OK || wifi_status.state != H2_PAL_WIFI_STA_STATE_GOT_IP) {
    memset(&saved, 0, sizeof(saved));
    rc = h2_pal_wifi_settings_get_saved_sta_config(
        state->runtime->wifi_settings, &saved);
    h2_smoke_wifi_csi_log(state, "saved_wifi", rc, (int)saved.ssid_len);
    if (rc != H2_PAL_OK) {
      h2_smoke_wifi_csi_set_status(
          state, H2_SMOKE_WIFI_CSI_STATUS_NO_SAVED_WIFI, now_ms + retry_ms);
      return;
    }
    h2_smoke_wifi_csi_set_status(state, H2_SMOKE_WIFI_CSI_STATUS_CONNECTING,
                                 now_ms + retry_ms);
    rc = h2_pal_wifi_sta_connect(state->runtime->wifi_sta, &saved,
                                 wifi_timeout_ms);
    h2_smoke_wifi_csi_log(state, "wifi_connect", rc, 0);
    if (h2_pal_time_get_monotonic_ms(state->runtime->time, &now_ms) !=
        H2_PAL_OK)
      now_ms += wifi_timeout_ms;
    if (rc != H2_PAL_OK) {
      h2_smoke_wifi_csi_set_status(state, H2_SMOKE_WIFI_CSI_STATUS_BACKOFF,
                                   now_ms + retry_ms);
      return;
    }
    memset(&wifi_status, 0, sizeof(wifi_status));
    rc = h2_pal_wifi_sta_get_status(state->runtime->wifi_sta, &wifi_status);
    h2_smoke_wifi_csi_log(state, "wifi_ready", rc, (int)wifi_status.state);
    if (rc != H2_PAL_OK || wifi_status.state != H2_PAL_WIFI_STA_STATE_GOT_IP) {
      h2_smoke_wifi_csi_set_status(state, H2_SMOKE_WIFI_CSI_STATUS_BACKOFF,
                                   now_ms + retry_ms);
      return;
    }
  }

  memset(&capabilities, 0, sizeof(capabilities));
  rc =
      h2_pal_wifi_csi_get_capabilities(state->runtime->wifi_csi, &capabilities);
  h2_smoke_wifi_csi_log(state, "capabilities", rc,
                        (int)capabilities.provider);
  if (rc != H2_PAL_OK) {
    h2_smoke_wifi_csi_set_error(
        state,
        rc == H2_PAL_ERR_UNSUPPORTED ? H2_SMOKE_WIFI_CSI_STATUS_UNSUPPORTED
                                     : H2_SMOKE_WIFI_CSI_STATUS_PROVIDER_ERROR,
        H2_SMOKE_WIFI_CSI_ERROR_CAPABILITIES, rc, now_ms + retry_ms);
    return;
  }
  memset(&csi_config, 0, sizeof(csi_config));
  csi_config.bssid_set = wifi_status.bssid_set;
  memcpy(csi_config.bssid, wifi_status.bssid, sizeof(csi_config.bssid));
  csi_config.min_delivery_interval_ms = 100u;
  frame_count_before_start = state->frame_count;
  rc = h2_pal_wifi_csi_start(state->runtime->wifi_csi, &csi_config,
                             h2_smoke_wifi_csi_frame_received, state);
  h2_smoke_wifi_csi_log(state, "start", rc,
                        (int)capabilities.max_sample_count);
  if (rc != H2_PAL_OK) {
    h2_smoke_wifi_csi_set_error(
        state,
        rc == H2_PAL_ERR_UNSUPPORTED ? H2_SMOKE_WIFI_CSI_STATUS_UNSUPPORTED
                                     : H2_SMOKE_WIFI_CSI_STATUS_PROVIDER_ERROR,
        H2_SMOKE_WIFI_CSI_ERROR_START, rc, now_ms + retry_ms);
    return;
  }
  if (h2_pal_mutex_lock(state->runtime->sync, state->mutex) == H2_PAL_OK) {
    state->provider = capabilities.provider;
    state->csi_started = 1;
    state->error_stage = H2_SMOKE_WIFI_CSI_ERROR_NONE;
    state->last_error = H2_PAL_OK;
    if (state->frame_count == frame_count_before_start) {
      state->status = H2_SMOKE_WIFI_CSI_STATUS_NO_FRAMES;
    }
    state->next_attempt_ms = now_ms + retry_ms;
    (void)h2_pal_mutex_unlock(state->runtime->sync, state->mutex);
  }
}

int h2_smoke_wifi_csi_run(h2_runtime_t *runtime,
                          const h2_smoke_wifi_csi_config_t *config) {
  h2_smoke_wifi_csi_state_t state;
  h2_smoke_wifi_csi_dashboard_t dashboard;
  h2_smoke_wifi_csi_dashboard_t rendered_dashboard;
  h2_display_info_t info;
  uint16_t *rows;
  h2_pal_mutex_config_t mutex_config;
  uint32_t wifi_timeout_ms;
  uint32_t retry_ms;
  uint32_t refresh_ms;
  uint64_t next_probe_ms = 0u;
  int rendered_dashboard_valid = 0;
  int rc;

  if (runtime == NULL || config == NULL || runtime->display == NULL ||
      runtime->mem == NULL || runtime->sync == NULL || runtime->time == NULL ||
      runtime->wifi_sta == NULL || runtime->wifi_settings == NULL ||
      runtime->wifi_csi == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(&state, 0, sizeof(state));
  state.runtime = runtime;
  memset(&mutex_config, 0, sizeof(mutex_config));
  mutex_config.name = "wifi-csi-smoke";
  mutex_config.allocator = runtime->mem;
  rc = h2_pal_mutex_create(runtime->sync, &mutex_config, &state.mutex);
  if (rc != H2_PAL_OK)
    return rc;
  rc = h2_pal_display_open(runtime->display);
  if (rc != H2_DISPLAY_OK)
    goto cleanup_mutex;
  rc = h2_pal_display_get_info(runtime->display, &info);
  if (rc != H2_DISPLAY_OK || info.width <= 0 || info.height <= 0) {
    rc = rc == H2_DISPLAY_OK ? H2_DISPLAY_ERR_INVALID_ARG : rc;
    goto cleanup_display;
  }
  rows = h2_pal_mem_alloc(runtime->mem, (size_t)info.width *
                                            H2_SMOKE_WIFI_CSI_RENDER_ROWS *
                                            sizeof(*rows));
  if (rows == NULL) {
    rc = H2_PAL_ERR_NO_MEMORY;
    goto cleanup_display;
  }
  (void)h2_pal_display_set_brightness_percent(runtime->display, 90u);
  wifi_timeout_ms = config->wifi_connect_timeout_ms == 0u
                        ? H2_SMOKE_WIFI_CSI_DEFAULT_WIFI_TIMEOUT_MS
                        : config->wifi_connect_timeout_ms;
  retry_ms = config->retry_interval_ms == 0u
                 ? H2_SMOKE_WIFI_CSI_DEFAULT_RETRY_MS
                 : config->retry_interval_ms;
  refresh_ms = config->refresh_interval_ms == 0u
                   ? H2_SMOKE_WIFI_CSI_DEFAULT_REFRESH_MS
                   : config->refresh_interval_ms;
  rc = h2_smoke_wifi_csi_snapshot(&state, &dashboard);
  if (rc == H2_PAL_OK)
    rc = h2_smoke_wifi_csi_render(runtime->display, &dashboard, &info, rows);
  if (config->ready != NULL)
    config->ready(config->user, rc);
  if (rc != H2_DISPLAY_OK)
    goto cleanup_rows;
  rendered_dashboard = dashboard;
  rendered_dashboard_valid = 1;

  for (;;) {
    uint64_t now_ms = 0u;
    uint64_t last_frame_ms = 0u;
    uint64_t next_attempt_ms = 0u;
    int csi_started = 0;
    (void)h2_pal_time_get_monotonic_ms(runtime->time, &now_ms);
    rc = h2_smoke_wifi_csi_get_progress(&state, &csi_started, &last_frame_ms,
                                        &next_attempt_ms);
    if (rc != H2_PAL_OK)
      break;
    if (csi_started) {
      h2_pal_wifi_sta_status_t wifi_status;
      memset(&wifi_status, 0, sizeof(wifi_status));
      rc = h2_pal_wifi_sta_get_status(runtime->wifi_sta, &wifi_status);
      if (rc != H2_PAL_OK ||
          wifi_status.state != H2_PAL_WIFI_STA_STATE_GOT_IP) {
        h2_smoke_wifi_csi_stop_for_retry(&state, now_ms + retry_ms);
        csi_started = 0;
      } else if (runtime->net != NULL && wifi_status.ip_valid != 0u &&
                 wifi_status.ip.gateway4 != 0u && now_ms >= next_probe_ms) {
        h2_pal_net_addr_t gateway;
        h2_pal_net_icmp_echo_result_t probe;
        memset(&gateway, 0, sizeof(gateway));
        gateway.family = H2_PAL_NET_FAMILY_IPV4;
        h2_pal_wifi_ip4_to_bytes(wifi_status.ip.gateway4, gateway.ip);
        memset(&probe, 0, sizeof(probe));
        (void)h2_pal_net_icmp_echo(runtime->net, &gateway, NULL,
                                   H2_SMOKE_WIFI_CSI_PROBE_TIMEOUT_MS, &probe);
        next_probe_ms = now_ms + H2_SMOKE_WIFI_CSI_PROBE_INTERVAL_MS;
      }
    }
    if (!csi_started && now_ms >= next_attempt_ms) {
      h2_smoke_wifi_csi_try_start(&state, now_ms, wifi_timeout_ms, retry_ms);
    }
    if (csi_started && last_frame_ms > 0u &&
        now_ms - last_frame_ms > H2_SMOKE_WIFI_CSI_NO_FRAME_MS) {
      h2_smoke_wifi_csi_set_status(&state, H2_SMOKE_WIFI_CSI_STATUS_NO_FRAMES,
                                   next_attempt_ms);
    }
    rc = h2_smoke_wifi_csi_snapshot(&state, &dashboard);
    if (rc != H2_PAL_OK)
      break;
    if (rendered_dashboard_valid &&
        h2_smoke_wifi_csi_dashboard_equal(&dashboard, &rendered_dashboard)) {
      rc = H2_DISPLAY_OK;
    } else {
      rc = h2_smoke_wifi_csi_render(runtime->display, &dashboard, &info, rows);
      if (rc == H2_DISPLAY_OK) {
        rendered_dashboard = dashboard;
        rendered_dashboard_valid = 1;
      }
    }
    if (rc != H2_DISPLAY_OK)
      break;
    if (config->should_stop != NULL && config->should_stop(config->user) != 0) {
      rc = H2_PAL_OK;
      break;
    }
    (void)h2_pal_time_sleep_ms(runtime->time, refresh_ms);
  }
cleanup_rows:
  if (state.csi_started)
    (void)h2_pal_wifi_csi_stop(runtime->wifi_csi);
  h2_pal_mem_free(runtime->mem, rows);
cleanup_display:
  (void)h2_pal_display_close(runtime->display);
cleanup_mutex:
  (void)h2_pal_mutex_destroy(runtime->sync, state.mutex);
  return rc;
}
