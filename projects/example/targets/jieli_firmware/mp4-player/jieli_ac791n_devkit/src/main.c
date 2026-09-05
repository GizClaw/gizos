#include "app_config.h"

#include "asm/includes.h"
#include "asm/wdt.h"
#include "h2_jieli_ac791n_devkit.h"
#include "h2_jieli_wl82_platform_core.h"
#include "h2_mp4_decoder.h"
#include "h2_tinyh264.h"
#include "os/os_api.h"

#include <stddef.h>
#include <stdint.h>

extern int printf(const char *format, ...);
extern uint32_t get_malloc_remain_heap_size(void);

typedef struct file_source {
  const h2_pal_fs_api_t *fs;
  h2_pal_fs_file_t *file;
  uint64_t position;
} file_source_t;

static OS_MUTEX log_mutex;
static int log_mutex_ready;
static const char *volatile current_stage = "boot";
static volatile int current_result = H2_PAL_OK;
static volatile unsigned decoded_frames;

void h2_jieli_usb_debug_lock(void) {
  if (log_mutex_ready) (void)os_mutex_pend(&log_mutex, 0u);
}

void h2_jieli_usb_debug_unlock(void) {
  if (log_mutex_ready) (void)os_mutex_post(&log_mutex);
}

static void trace(const char *stage, int result) {
  current_stage = stage;
  current_result = result;
  if (log_mutex_ready) (void)os_mutex_pend(&log_mutex, 0u);
  (void)printf(
      "H2_MP4_FIRST_FRAME stage=%s rc=%d heap_free=%u\r\n",
      stage, result, (unsigned)get_malloc_remain_heap_size());
  if (log_mutex_ready) (void)os_mutex_post(&log_mutex);
  os_time_dly(20u);
}

void h2_mp4_decoder_trace_stage(const char *stage) {
  trace(stage, H2_PAL_OK);
}

void h2_tinyh264_trace_stage(const char *stage) {
  trace(stage, H2_PAL_OK);
}

int h2_jieli_ac791n_devkit_early_app_boot(void) {
  wdt_close();
  return H2_PAL_OK;
}

static h2_pal_result_t file_read_at(
    void *user,
    uint64_t offset,
    void *buffer,
    size_t capacity,
    size_t *out_read) {
  file_source_t *source = user;
  if (offset != source->position) {
    h2_pal_result_t result = (h2_pal_result_t)h2_pal_fs_seek(
        source->fs, source->file, offset);
    if (result != H2_PAL_OK) return result;
  }
  h2_pal_result_t result = (h2_pal_result_t)h2_pal_fs_read(
      source->fs, source->file, buffer, capacity, out_read);
  if (result == H2_PAL_OK) source->position = offset + *out_read;
  return result;
}

static void heartbeat(void *user) {
  (void)user;
  unsigned sequence = 0u;
  for (;;) {
    os_time_dly(200u);
    if (log_mutex_ready) (void)os_mutex_pend(&log_mutex, 0u);
    (void)printf(
        "H2_MP4_HEARTBEAT seq=%u stage=%s rc=%d frames=%u heap_free=%u\r\n",
        ++sequence, current_stage, current_result,
        decoded_frames,
        (unsigned)get_malloc_remain_heap_size());
    if (log_mutex_ready) (void)os_mutex_post(&log_mutex);
  }
}

static void decode_first_frame(void *user) {
  (void)user;
  trace("startup-delay", H2_PAL_OK);
  os_time_dly(500u);

  h2_pal_fs_api_t fs = {0};
  h2_pal_result_t result = h2_jieli_ac791n_devkit_sd_fs_init(&fs);
  trace("fs-init-return", result);
  if (result != H2_PAL_OK) goto stopped;

  static const char path[] = "/data/media/startup.mp4";
  h2_pal_fs_stat_t stat = {0};
  result = (h2_pal_result_t)h2_pal_fs_stat(&fs, path, &stat);
  trace("file-stat-return", result);
  if (result != H2_PAL_OK || stat.is_dir || stat.size == 0u) goto stopped;

  h2_pal_fs_file_t *file = NULL;
  result = (h2_pal_result_t)h2_pal_fs_open(
      &fs, path, H2_PAL_FS_OPEN_READ, &file);
  trace("file-open-return", result);
  if (result != H2_PAL_OK) goto stopped;

  file_source_t source = {.fs = &fs, .file = file};
  const h2_mp4_decoder_config_t config = {
      .allocator = h2_jieli_wl82_platform_mem_api(),
      .source = {
          .user = &source,
          .size = stat.size,
          .read_at = file_read_at,
      },
      .video_decoder = *h2_tinyh264_video_decoder_api(),
      .video_format = H2_VIDEO_PIXEL_FORMAT_RGB565,
      .require_video = 1,
      .require_audio = 0,
  };
  h2_mp4_decoder_t *decoder = NULL;
  trace("mp4-open-enter", H2_PAL_OK);
  result = h2_mp4_decoder_open(&config, &decoder);
  trace("mp4-open-return", result);
  if (result != H2_PAL_OK) goto stopped;

  h2_mp4_decoder_info_t media = {0};
  result = h2_mp4_decoder_get_info(decoder, &media);
  trace("media-info-return", result);
  if (result != H2_PAL_OK) goto stopped;

  trace("video-loop-enter", H2_PAL_OK);
  for (;;) {
    h2_mp4_decoder_frame_t *frame = NULL;
    result = h2_mp4_decoder_acquire_frame(decoder, 2000u, &frame);
    if (result == H2_PAL_EXIT) {
      result = H2_PAL_OK;
      trace("video-eos", result);
      break;
    }
    if (result != H2_PAL_OK) {
      trace("video-loop-fail", result);
      break;
    }

    h2_mp4_decoder_frame_info_t frame_info = {0};
    result = h2_mp4_decoder_frame_get_info(decoder, frame, &frame_info);
    if (result == H2_PAL_OK) {
      result = h2_mp4_decoder_release_frame(decoder, frame);
    }
    if (result != H2_PAL_OK) {
      trace("video-frame-fail", result);
      break;
    }
    ++decoded_frames;
    if (decoded_frames == 1u || decoded_frames % 10u == 0u) {
      if (log_mutex_ready) (void)os_mutex_pend(&log_mutex, 0u);
      (void)printf(
          "H2_MP4_VIDEO_PROGRESS frames=%u width=%u height=%u pts_us=%lld\r\n",
          decoded_frames,
          (unsigned)frame_info.width,
          (unsigned)frame_info.height,
          (long long)frame_info.pts_us);
      if (log_mutex_ready) (void)os_mutex_post(&log_mutex);
    }
  }

stopped:
  for (;;) os_time_dly(100u);
}

void app_main(void) {
  log_mutex_ready = os_mutex_create(&log_mutex) == 0;
  int result = h2_jieli_ac791n_devkit_usb_debug_start();
  trace("usb-debug", result);
  if (result != 0) return;

  result = os_task_create(
      heartbeat, NULL, 10, 2048, 0, "h2mp4/heartbeat");
  trace("heartbeat-task", result);
  if (result != OS_NO_ERR) return;

  result = os_task_create(
      decode_first_frame, NULL, 10, 8192, 0, "h2mp4/decode");
  trace("decode-task", result);
}
