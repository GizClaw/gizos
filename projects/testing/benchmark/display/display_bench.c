#include "display_bench.h"

#include <stdio.h>

#define BENCH_WARMUP 8u
#define BENCH_SAMPLES 64u
#define BENCH_REGION 128
#define BENCH_MIB (1024u * 1024u)

static void unavailable(const h2_display_bench_profile_t* profile,
                        const char* shape, const char* metric,
                        const char* stage, int rc, unsigned count) {
  char line[256];
  snprintf(line, sizeof(line),
           "H2_DISPLAY_BENCH_RESULT shape=%s metric=%s status=unavailable "
           "stage=%s rc=%d completed_samples=%u",
           shape, metric, stage, rc, count);
  profile->line(line);
}

static void format_rates(char* out, size_t capacity, uint64_t total_us,
                         unsigned bytes, const char* payload_name,
                         const char* rate_name) {
  const uint64_t denom = BENCH_MIB * total_us;
  const uint64_t mib_milli =
      ((uint64_t)bytes * BENCH_SAMPLES * 1000000000u + denom / 2u) / denom;
  const uint64_t rate_milli =
      (BENCH_SAMPLES * UINT64_C(1000000000) + total_us / 2u) / total_us;
  int used = snprintf(out, capacity, " %s=%lu.%03u", payload_name,
                      (unsigned long)(mib_milli / 1000u),
                      (unsigned)(mib_milli % 1000u));
  if (rate_name != NULL && used >= 0 && (size_t)used < capacity)
    snprintf(out + used, capacity - (size_t)used, " %s=%lu.%03u", rate_name,
             (unsigned long)(rate_milli / 1000u),
             (unsigned)(rate_milli % 1000u));
}

static int report(const h2_display_bench_profile_t* profile, const char* shape,
                  const char* metric, const h2_display_rect_t* rect,
                  uint32_t samples[BENCH_SAMPLES], const char* payload_name,
                  const char* rate_name) {
  uint64_t total_us = 0;
  for (unsigned i = 0; i < BENCH_SAMPLES; ++i) {
    const uint32_t sample = samples[i];
    total_us += sample;
    unsigned j = i;
    for (; j > 0 && samples[j - 1] > sample; --j) samples[j] = samples[j - 1];
    samples[j] = sample;
  }
  if (total_us == 0) {
    unavailable(profile, shape, metric, "clock_resolution", H2_PAL_ERR_IO,
                BENCH_SAMPLES);
    return H2_PAL_ERR_IO;
  }
  const unsigned bytes =
      (unsigned)rect->width * rect->height * sizeof(uint16_t);
  unsigned values_us[] = {
      samples[0],
      samples[(BENCH_SAMPLES + 1u) / 2u - 1u],
      (unsigned)((total_us + BENCH_SAMPLES / 2u) / BENCH_SAMPLES),
      samples[(BENCH_SAMPLES * 95u + 99u) / 100u - 1u],
      samples[BENCH_SAMPLES - 1u],
  };
  char rates[128];
  format_rates(rates, sizeof(rates), total_us, bytes, payload_name, rate_name);
  char line[512];
  snprintf(line, sizeof(line),
           "H2_DISPLAY_BENCH_RESULT shape=%s metric=%s status=ok "
           "w=%d h=%d payload_bytes=%u n=%u elapsed_us=%llu "
           "min_ms=%u.%03u p50_ms=%u.%03u mean_ms=%u.%03u p95_ms=%u.%03u "
           "max_ms=%u.%03u%s",
           shape, metric, rect->width, rect->height, bytes, BENCH_SAMPLES,
           (unsigned long long)total_us, values_us[0] / 1000u,
           values_us[0] % 1000u, values_us[1] / 1000u, values_us[1] % 1000u,
           values_us[2] / 1000u, values_us[2] % 1000u, values_us[3] / 1000u,
           values_us[3] % 1000u, values_us[4] / 1000u, values_us[4] % 1000u,
           rates);
  profile->line(line);
  return H2_PAL_OK;
}

static int update(const h2_display_bench_profile_t* profile,
                  const h2_display_rect_t* rect, const uint16_t* pixels) {
  const size_t stride = (size_t)rect->width * sizeof(uint16_t);
  const uint32_t before =
      profile->completed_pixels == NULL ? 0u : profile->completed_pixels();
  int rc = h2_pal_display_draw_bitmap(profile->display, rect, pixels, stride,
                                      H2_DISPLAY_PIXEL_RGB565);
  if (rc == H2_DISPLAY_OK) rc = h2_pal_display_present(profile->display);
  if (profile->timed_out != NULL && profile->timed_out())
    return H2_PAL_ERR_TIMEOUT;
  if (rc == H2_DISPLAY_OK && profile->completed_pixels != NULL &&
      profile->completed_pixels() - before !=
          (uint32_t)rect->width * rect->height)
    return H2_DISPLAY_ERR_IO;
  return rc;
}

static int run_shape(const h2_display_bench_profile_t* profile,
                     const h2_display_info_t* panel, bool full) {
  const char* shape = full ? "full" : "region";
  const h2_display_rect_t rect = {0, 0, full ? panel->width : BENCH_REGION,
                                  full ? panel->height : BENCH_REGION};
  const bool transfer = profile->mode == H2_DISPLAY_BENCH_SYNC_TRANSFER;
  const bool copying = profile->mode == H2_DISPLAY_BENCH_COPY;
  const bool close_fence = profile->mode == H2_DISPLAY_BENCH_CLOSE_FENCE;
  const char* metric = transfer  ? "qspi_dma_complete"
                       : copying ? "framebuffer_copy"
                                 : "app_draw_present";
  const char* completed_rate = full ? "fps" : "updates_hz";
  const size_t pixel_count = (size_t)rect.width * rect.height;
  uint16_t* pixels =
      h2_pal_mem_alloc(profile->memory, pixel_count * 2u * sizeof(uint16_t));
  const char* stage = "allocation";
  unsigned measured = 0;
  int rc = H2_PAL_ERR_NO_MEMORY;
  if (pixels == NULL) goto failed;

  for (int y = 0; y < rect.height; ++y) {
    for (int x = 0; x < rect.width; ++x) {
      const size_t index = (size_t)y * rect.width + x;
      const bool stripe = ((x / 32) ^ (y / 32)) & 1;
      pixels[index] = stripe ? 0x7800u : 0x03e0u;
      pixels[pixel_count + index] = stripe ? 0x001fu : 0x7befu;
    }
  }

  stage = "open";
  rc = h2_pal_display_open(profile->display);
  if (rc != H2_DISPLAY_OK) goto failed;
  stage = "warmup";
  for (unsigned i = 0; i < BENCH_WARMUP; ++i) {
    rc = update(profile, &rect, pixels + ((i & 1u) ? pixel_count : 0u));
    if (rc != H2_DISPLAY_OK) goto failed;
  }
  if (close_fence) {
    /* Drain the final warmup before starting the measured EMI batch. */
    stage = "warmup_close";
    rc = h2_pal_display_close(profile->display);
    if (rc != H2_DISPLAY_OK) goto failed;
    stage = "reopen";
    rc = h2_pal_display_open(profile->display);
    if (rc != H2_DISPLAY_OK) goto failed;
  }

  uint32_t samples[BENCH_SAMPLES];
  uint64_t first_us = 0, last_us = 0;
  for (; measured < BENCH_SAMPLES; ++measured) {
    uint64_t begin = 0;
    stage = "clock";
    rc = h2_pal_time_get_monotonic_us(profile->time, &begin);
    if (rc != H2_PAL_OK) goto failed;
    if (measured == 0u) first_us = begin;
    stage = "draw_present";
    rc = update(profile, &rect, pixels + ((measured & 1u) ? pixel_count : 0u));
    if (rc != H2_DISPLAY_OK) goto failed;
    stage = "clock";
    rc = h2_pal_time_get_monotonic_us(profile->time, &last_us);
    if (rc != H2_PAL_OK) goto failed;
    if (last_us < begin || last_us - begin > UINT32_MAX) {
      rc = H2_PAL_ERR_IO;
      goto failed;
    }
    samples[measured] = (uint32_t)(last_us - begin);
  }

  uint64_t closed_us = 0;
  if (close_fence) {
    stage = "close_fence";
    rc = h2_pal_display_close(profile->display);
    if (rc != H2_DISPLAY_OK) goto failed;
    rc = h2_pal_time_get_monotonic_us(profile->time, &closed_us);
    if (rc != H2_PAL_OK) goto failed;
    if (closed_us < last_us || closed_us <= first_us ||
        closed_us - first_us > UINT32_MAX) {
      rc = H2_PAL_ERR_IO;
      goto failed;
    }
  }
  h2_pal_mem_free(profile->memory, pixels);
  rc = report(profile, shape, metric, &rect, samples,
              transfer ? "payload_mib_s" : "logical_payload_mib_s",
              transfer  ? completed_rate
              : copying ? NULL
                        : "submissions_hz");
  if (close_fence) {
    char rates[128], line[410];
    const unsigned bytes = (unsigned)(pixel_count * sizeof(uint16_t));
    const uint64_t batch_us = closed_us - first_us;
    format_rates(rates, sizeof(rates), batch_us, bytes, "payload_mib_s",
                 completed_rate);
    snprintf(
        line, sizeof(line),
        "H2_DISPLAY_BENCH_RESULT shape=%s metric=emi_batch_drained status=ok "
        "w=%d h=%d payload_bytes=%u n=%u elapsed_us=%lu close_us=%lu%s",
        shape, rect.width, rect.height, bytes, BENCH_SAMPLES,
        (unsigned long)batch_us, (unsigned long)(closed_us - last_us), rates);
    profile->line(line);
  }
  if (!transfer)
    unavailable(profile, shape, "per_update_transfer", "no_public_fence",
                H2_PAL_ERR_UNSUPPORTED, 0u);
  return rc;

failed:
  h2_pal_mem_free(profile->memory, pixels);
  unavailable(profile, shape, metric, stage, rc, measured);
  if (close_fence)
    unavailable(profile, shape, "emi_batch_drained", stage, rc, 0u);
  return rc;
}

int h2_display_bench_run(const h2_display_bench_profile_t* profile) {
  char line[256];
  snprintf(line, sizeof(line),
           "H2_DISPLAY_BENCH_BOOT board=%s version=%s bus=%s "
           "format=RGB565 warmup=%u samples=%u clock_tick_ns=%u",
           profile->board, profile->version, profile->bus, BENCH_WARMUP,
           BENCH_SAMPLES, profile->clock_tick_ns);
  profile->line(line);
  h2_display_info_t info = {0};
  int rc = h2_pal_display_open(profile->display);
  if (rc == H2_DISPLAY_OK)
    rc = h2_pal_display_get_info(profile->display, &info);
  if (rc == H2_DISPLAY_OK &&
      (info.native_format != H2_DISPLAY_PIXEL_RGB565 ||
       info.width < BENCH_REGION || info.height < BENCH_REGION ||
       info.width > 4096 || info.height > 4096))
    rc = H2_DISPLAY_ERR_UNSUPPORTED;
  if (rc == H2_DISPLAY_OK) {
    snprintf(line, sizeof(line), "H2_DISPLAY_BENCH_PANEL w=%d h=%d %s",
             info.width, info.height, profile->panel_config);
    profile->line(line);
    rc = run_shape(profile, &info, true);
    if (rc == H2_PAL_OK)
      rc = run_shape(profile, &info, false);
    else
      unavailable(profile, "region", "update", "full_failed", rc, 0u);
  } else {
    unavailable(profile, "full", "update", "display_open", rc, 0u);
    unavailable(profile, "region", "update", "display_open", rc, 0u);
  }
  unavailable(profile, "full", "scanout", "no_public_signal",
              H2_PAL_ERR_UNSUPPORTED, 0u);
  snprintf(line, sizeof(line), "H2_DISPLAY_BENCH_END rc=%d", rc);
  profile->line(line);
  return rc;
}
