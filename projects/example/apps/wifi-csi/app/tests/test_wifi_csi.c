#include "h2_smoke_wifi_csi.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TEST_ASSERT(condition)                                                 \
  do {                                                                         \
    if (!(condition))                                                          \
      abort();                                                                 \
  } while (0)

typedef struct test_mutex {
  int unused;
} test_mutex_t;

typedef struct test_state {
  int csi_supported;
  int csi_start_result;
  int emit_invalid_frame;
  int drop_link_after_start;
  int link_drop_reported;
  int csi_start_calls;
  int csi_stop_calls;
  int csi_stop_after_stop_calls;
  int ping_calls;
  int display_open_calls;
  int display_close_calls;
  int display_present_calls;
  int ready_calls;
  int stop_calls;
  int loop_limit;
  int saw_capturing;
  int saw_invalid_frame;
  int saw_no_frames;
  int saw_unsupported;
  int saw_waveform;
  int saw_provider_error_code;
  int waveform_min_y[160];
  int waveform_max_y[160];
} test_state_t;

static void *test_alloc(void *user, size_t size) {
  (void)user;
  return malloc(size);
}

static void test_free(void *user, void *ptr) {
  (void)user;
  free(ptr);
}

static h2_pal_result_t test_mutex_create(void *user,
                                         const h2_pal_mutex_config_t *config,
                                         h2_pal_mutex_t **out_mutex) {
  static test_mutex_t mutex;
  (void)user;
  TEST_ASSERT(config != NULL && out_mutex != NULL);
  *out_mutex = (h2_pal_mutex_t *)&mutex;
  return H2_PAL_OK;
}

static h2_pal_result_t test_mutex_destroy(void *user, h2_pal_mutex_t *mutex) {
  (void)user;
  TEST_ASSERT(mutex != NULL);
  return H2_PAL_OK;
}

static h2_pal_result_t test_mutex_lock(void *user, h2_pal_mutex_t *mutex) {
  (void)user;
  TEST_ASSERT(mutex != NULL);
  return H2_PAL_OK;
}

static h2_pal_result_t test_mutex_unlock(void *user, h2_pal_mutex_t *mutex) {
  (void)user;
  TEST_ASSERT(mutex != NULL);
  return H2_PAL_OK;
}

static int test_display_open(void *user) {
  ((test_state_t *)user)->display_open_calls++;
  return H2_DISPLAY_OK;
}

static int test_display_get_info(void *user, h2_display_info_t *out_info) {
  (void)user;
  *out_info = (h2_display_info_t){
      .width = 160,
      .height = 120,
      .native_format = H2_DISPLAY_PIXEL_RGB565,
  };
  return H2_DISPLAY_OK;
}

static int test_display_draw(void *user, const h2_display_rect_t *rect,
                             const void *pixels, size_t stride_bytes,
                             h2_display_pixel_format_t format) {
  test_state_t *state = user;
  const uint16_t *rows = pixels;
  (void)stride_bytes;
  TEST_ASSERT(rect != NULL && format == H2_DISPLAY_PIXEL_RGB565);
  for (int y = 0; y < rect->height; ++y) {
    for (int x = 0; x < rect->width; ++x) {
      uint16_t pixel = rows[(size_t)y * (size_t)rect->width + (size_t)x];
      if (pixel == 0x07e0u) {
        state->saw_capturing = 1;
      }
      if (pixel == 0xf800u) {
        state->saw_invalid_frame = 1;
      }
      if (pixel == 0x07ffu) {
        state->saw_waveform = 1;
        int pixel_y = rect->y + y;
        if (pixel_y < state->waveform_min_y[x])
          state->waveform_min_y[x] = pixel_y;
        if (pixel_y > state->waveform_max_y[x])
          state->waveform_max_y[x] = pixel_y;
      }
      if (x == 9 && rect->y + y == 25 && pixel == 0xffe0u) {
        state->saw_no_frames = 1;
      }
      /* C is lit here while the I in "INVALID FRAME" is not. */
      if (x == 8 && rect->y + y == 27 && pixel == 0xf800u) {
        state->saw_unsupported = 1;
      }
      /* START--6: first lit pixel of the final digit. */
      if (x == 92 && rect->y + y == 89 && pixel == 0xffffu) {
        state->saw_provider_error_code = 1;
      }
    }
  }
  return H2_DISPLAY_OK;
}

static int test_display_present(void *user) {
  ((test_state_t *)user)->display_present_calls++;
  return H2_DISPLAY_OK;
}

static int test_display_close(void *user) {
  ((test_state_t *)user)->display_close_calls++;
  return H2_DISPLAY_OK;
}

static h2_pal_result_t test_time_monotonic(void *user, uint64_t *out_ms) {
  test_state_t *state = user;
  *out_ms = 100u + (uint64_t)state->stop_calls * 250u;
  return H2_PAL_OK;
}

static h2_pal_result_t test_time_sleep(void *user, uint32_t ms) {
  (void)user;
  (void)ms;
  return H2_PAL_OK;
}

static int test_wifi_status(void *user, h2_pal_wifi_sta_status_t *out_status) {
  test_state_t *state = user;
  memset(out_status, 0, sizeof(*out_status));
  if (state->drop_link_after_start && state->csi_start_calls > 0 &&
      !state->link_drop_reported) {
    state->link_drop_reported = 1;
    return H2_PAL_OK;
  }
  out_status->state = H2_PAL_WIFI_STA_STATE_GOT_IP;
  out_status->bssid_set = 1u;
  out_status->bssid[0] = 1u;
  out_status->ip_valid = 1u;
  out_status->ip.gateway4 = 0xc0a80101u;
  return H2_PAL_OK;
}

static h2_pal_result_t
test_icmp_echo(void *user, const h2_pal_net_addr_t *addr,
               const h2_pal_net_bind_t *bind, uint32_t timeout_ms,
               h2_pal_net_icmp_echo_result_t *out_result) {
  test_state_t *state = user;
  TEST_ASSERT(addr != NULL && addr->family == H2_PAL_NET_FAMILY_IPV4);
  TEST_ASSERT(addr->ip[0] == 192u && addr->ip[1] == 168u);
  TEST_ASSERT(addr->ip[2] == 1u && addr->ip[3] == 1u);
  TEST_ASSERT(bind == NULL && timeout_ms == 100u && out_result != NULL);
  state->ping_calls++;
  *out_result = (h2_pal_net_icmp_echo_result_t){
      .elapsed_ms = 1u,
      .transmitted = 1u,
      .received = 1u,
  };
  return H2_PAL_OK;
}

static h2_pal_result_t
test_csi_capabilities(void *user,
                      h2_pal_wifi_csi_capabilities_t *out_capabilities) {
  test_state_t *state = user;
  out_capabilities->provider = state->csi_supported
                                   ? H2_PAL_WIFI_CSI_PROVIDER_ESP_IDF
                                   : H2_PAL_WIFI_CSI_PROVIDER_BK7258;
  out_capabilities->max_sample_count = 16u;
  return state->csi_supported ? H2_PAL_OK : H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t test_csi_start(void *user,
                                      const h2_pal_wifi_csi_config_t *config,
                                      h2_pal_wifi_csi_frame_fn frame_cb,
                                      void *frame_user) {
  test_state_t *state = user;
  const h2_pal_wifi_csi_sample_t samples[] = {
      {.real = 0, .imag = 0},
      {.real = 127, .imag = 127},
  };
  const h2_pal_wifi_csi_frame_t frame = {
      .provider = H2_PAL_WIFI_CSI_PROVIDER_ESP_IDF,
      .phy = H2_PAL_WIFI_CSI_PHY_HT,
      .channel = 6u,
      .bandwidth_mhz = 20u,
      .mcs = 0u,
      .rssi_dbm = -45,
      .samples = samples,
      .sample_count = sizeof(samples) / sizeof(samples[0]),
  };

  TEST_ASSERT(config != NULL && config->bssid_set == 1u && frame_cb != NULL);
  state->csi_start_calls++;
  if (state->csi_start_result != H2_PAL_OK)
    return state->csi_start_result;
  if (!state->drop_link_after_start || state->csi_start_calls == 1)
    frame_cb(frame_user, &frame);
  if (state->emit_invalid_frame) {
    h2_pal_wifi_csi_frame_t invalid_frame = frame;
    invalid_frame.samples = NULL;
    invalid_frame.sample_count = 0u;
    frame_cb(frame_user, &invalid_frame);
  }
  return H2_PAL_OK;
}

static h2_pal_result_t test_csi_stop(void *user) {
  test_state_t *state = user;
  state->csi_stop_calls++;
  state->csi_stop_after_stop_calls = state->stop_calls;
  return H2_PAL_OK;
}

static void test_ready(void *user, int result) {
  test_state_t *state = user;
  TEST_ASSERT(result == H2_DISPLAY_OK);
  state->ready_calls++;
}

static int test_should_stop(void *user) {
  test_state_t *state = user;
  state->stop_calls++;
  return state->stop_calls >= state->loop_limit;
}

static void run_test(int csi_supported, int emit_invalid_frame,
                     int drop_link_after_start, int csi_start_result) {
  test_state_t state = {
      .csi_supported = csi_supported,
      .csi_start_result = csi_start_result,
      .emit_invalid_frame = emit_invalid_frame,
      .drop_link_after_start = drop_link_after_start,
      .loop_limit = csi_supported ? 2 : 3,
  };
  for (size_t x = 0u; x < 160u; ++x) {
    state.waveform_min_y[x] = 120;
    state.waveform_max_y[x] = -1;
  }
  const h2_pal_mem_vtable_t mem_vtable = {
      .alloc = test_alloc,
      .free = test_free,
  };
  const h2_pal_mem_api_t mem = {.vtable = &mem_vtable};
  const h2_pal_sync_vtable_t sync_vtable = {
      .create_mutex = test_mutex_create,
      .destroy_mutex = test_mutex_destroy,
      .lock_mutex = test_mutex_lock,
      .try_lock_mutex = test_mutex_lock,
      .unlock_mutex = test_mutex_unlock,
  };
  const h2_pal_sync_api_t sync = {.vtable = &sync_vtable};
  const h2_pal_display_vtable_t display_vtable = {
      .open = test_display_open,
      .get_info = test_display_get_info,
      .draw_bitmap = test_display_draw,
      .present = test_display_present,
      .close = test_display_close,
  };
  const h2_pal_display_api_t display = {
      .user = &state,
      .vtable = &display_vtable,
  };
  const h2_pal_time_vtable_t time_vtable = {
      .get_monotonic_ms = test_time_monotonic,
      .sleep_ms = test_time_sleep,
  };
  const h2_pal_time_api_t time = {
      .user = &state,
      .vtable = &time_vtable,
  };
  const h2_pal_net_vtable_t net_vtable = {
      .icmp_echo = test_icmp_echo,
  };
  const h2_pal_net_api_t net = {
      .user = &state,
      .vtable = &net_vtable,
  };
  const h2_pal_wifi_sta_vtable_t wifi_sta_vtable = {
      .get_status = test_wifi_status,
  };
  const h2_pal_wifi_sta_api_t wifi_sta = {
      .user = &state,
      .vtable = &wifi_sta_vtable,
  };
  const h2_pal_wifi_settings_vtable_t wifi_settings_vtable = {0};
  const h2_pal_wifi_settings_api_t wifi_settings = {
      .vtable = &wifi_settings_vtable,
  };
  const h2_pal_wifi_csi_vtable_t wifi_csi_vtable = {
      .get_capabilities = test_csi_capabilities,
      .start = test_csi_start,
      .stop = test_csi_stop,
  };
  const h2_pal_wifi_csi_api_t wifi_csi = {
      .user = &state,
      .vtable = &wifi_csi_vtable,
  };
  h2_runtime_t runtime = {
      .display = &display,
      .mem = &mem,
      .net = &net,
      .sync = &sync,
      .time = &time,
      .wifi_sta = &wifi_sta,
      .wifi_settings = &wifi_settings,
      .wifi_csi = &wifi_csi,
  };
  const h2_smoke_wifi_csi_config_t config = {
      .user = &state,
      .ready = test_ready,
      .should_stop = test_should_stop,
      .retry_interval_ms = drop_link_after_start ? 1u : 0u,
  };

  TEST_ASSERT(h2_smoke_wifi_csi_run(&runtime, &config) == H2_PAL_OK);
  TEST_ASSERT(state.ready_calls == 1 && state.display_open_calls == 1);
  TEST_ASSERT(state.display_close_calls == 1 &&
              state.display_present_calls >= 2);
  if (csi_supported) {
    if (csi_start_result != H2_PAL_OK) {
      TEST_ASSERT(state.csi_start_calls == 1 && state.csi_stop_calls == 0);
      TEST_ASSERT(state.saw_capturing == 0 && state.saw_waveform == 0);
      TEST_ASSERT(state.saw_provider_error_code != 0);
      return;
    }
    TEST_ASSERT(state.csi_start_calls == (drop_link_after_start ? 2 : 1));
    TEST_ASSERT(state.csi_stop_calls == (drop_link_after_start ? 2 : 1));
    TEST_ASSERT(state.ping_calls == (drop_link_after_start ? 0 : 1));
    if (emit_invalid_frame) {
      TEST_ASSERT(state.saw_invalid_frame == 0);
      TEST_ASSERT(state.saw_capturing != 0);
      TEST_ASSERT(state.saw_waveform != 0);
    } else {
      TEST_ASSERT(state.saw_capturing != 0);
      TEST_ASSERT(state.saw_waveform != 0);
      for (int x = 9; x <= 151; ++x) {
        TEST_ASSERT(state.waveform_max_y[x] >= state.waveform_min_y[x]);
        TEST_ASSERT(state.waveform_max_y[x - 1] >= state.waveform_min_y[x]);
        TEST_ASSERT(state.waveform_max_y[x] >= state.waveform_min_y[x - 1]);
      }
    }
    if (drop_link_after_start) {
      TEST_ASSERT(state.csi_stop_after_stop_calls == 2);
      TEST_ASSERT(state.saw_no_frames != 0);
    }
  } else {
    TEST_ASSERT(state.csi_start_calls == 0 && state.csi_stop_calls == 0);
    TEST_ASSERT(state.ping_calls == 0);
    TEST_ASSERT(state.saw_unsupported != 0);
    TEST_ASSERT(state.display_present_calls == 2);
  }
}

int main(void) {
  run_test(1, 0, 0, H2_PAL_OK);
  run_test(1, 1, 0, H2_PAL_OK);
  run_test(1, 0, 1, H2_PAL_OK);
  run_test(1, 0, 0, H2_PAL_ERR_TIMEOUT);
  run_test(0, 0, 0, H2_PAL_OK);
  return 0;
}
