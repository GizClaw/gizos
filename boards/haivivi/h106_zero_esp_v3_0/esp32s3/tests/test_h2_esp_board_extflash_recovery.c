#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#define IRAM_ATTR
#include "h2_esp_board_extflash_recovery.h"

static uint8_t* image;
static esp_partition_t partition;
static uint8_t scratch[8192];
static size_t reads, fail_read, unstable_read;
static unsigned mounts, erases, formats, fail_erase;
static size_t erased_bytes;
static size_t scan_progress[16], erase_progress[16];
static size_t scan_progress_count, erase_progress_count;
static esp_err_t mount_result, erase_result, format_result, remount_result;
static h2_extflash_scan_info_t scan_info;
static h2_extflash_format_t scan_result;
static unsigned scan_reports;
static int scan_read_mismatch;
static size_t scan_failure_offset;

static void record_scan_result(h2_extflash_format_t format,
                               const h2_extflash_probe_t* probe) {
  assert(erases == 0u && formats == 0u && scan_reports == 0u);
  assert(probe->scan.scanned_bytes <= partition.size);
  ++scan_reports;
  scan_info = probe->scan;
  scan_result = format;
  scan_read_mismatch = probe->read_mismatch;
  scan_failure_offset = probe->failure_offset;
}

static void record_progress(const char* stage, size_t completed, size_t total) {
  assert(total == partition.size && completed <= total);
  size_t* values;
  size_t* count;
  if (strcmp(stage, "scan") == 0) {
    assert(erases == 0u);
    values = scan_progress;
    count = &scan_progress_count;
  } else {
    assert(strcmp(stage, "erase") == 0 && completed == erased_bytes);
    values = erase_progress;
    count = &erase_progress_count;
  }
  assert(formats == 0u && *count < 16u);
  assert(*count == 0u ? completed == 0u : completed > values[*count - 1u]);
  values[(*count)++] = completed;
}

esp_err_t esp_partition_read(const esp_partition_t* p, size_t off, void* dst,
                             size_t len) {
  assert(off <= p->size && len <= p->size - off && len <= 4096u);
  if (++reads == fail_read) {
    return ESP_FAIL;
  }
  memcpy(dst, image + off, len);
  if (reads == unstable_read) {
    ((uint8_t*)dst)[0] ^= 1u;
  }
  return ESP_OK;
}

esp_err_t esp_partition_erase_range(const esp_partition_t* p, size_t off,
                                    size_t len) {
  assert(off == erased_bytes && len <= 1024u * 1024u);
  assert(off <= p->size && len <= p->size - off && formats == 0u);
  assert(scan_progress_count > 0u &&
         scan_progress[scan_progress_count - 1u] == p->size);
  ++erases;
  const esp_err_t result = erases == fail_erase ? ESP_FAIL : erase_result;
  if (result == ESP_OK) {
    memset(image + off, 0xFF, len);
    erased_bytes += len;
  }
  return result;
}

esp_err_t esp_littlefs_format_partition(const esp_partition_t* p) {
  assert(p == &partition);
  ++formats;
  return format_result;
}

esp_err_t esp_vfs_littlefs_register(const esp_vfs_littlefs_conf_t* conf) {
  assert(!conf->format_if_mount_failed && !conf->dont_mount);
  assert(conf->partition == &partition);
  return ++mounts == 1u ? mount_result : remount_result;
}

static void reset(size_t size) {
  free(image);
  image = malloc(size);
  assert(image != NULL);
  memset(image, 0xFF, size);
  partition.size = size;
  reads = 0u;
  fail_read = 0u;
  unstable_read = 0u;
  mounts = 0u;
  erases = 0u;
  formats = 0u;
  fail_erase = 0u;
  erased_bytes = 0u;
  scan_progress_count = erase_progress_count = 0u;
  memset(&scan_info, 0, sizeof(scan_info));
  scan_result = H2_EXTFLASH_UNKNOWN;
  scan_reports = 0u;
  scan_read_mismatch = 0;
  scan_failure_offset = 0u;
  mount_result = ESP_FAIL;
  erase_result = format_result = remount_result = ESP_OK;
}
static h2_extflash_mount_call_t run(void) {
  h2_extflash_mount_call_t call = {
      .partition = &partition,
      .scratch = scratch,
      .base_path = "/ext",
      .mount_error = ESP_FAIL,
      .scan_error = ESP_OK,
      .erase_error = ESP_FAIL,
      .format_error = ESP_FAIL,
      .remount_error = ESP_FAIL,
      .progress = record_progress,
      .scan_report = record_scan_result,
  };
  mount_safe_callback(&call);
  return call;
}
static void preserved(void) {
  h2_extflash_mount_call_t call = run();
  assert(!call.mounted && !call.recovery_started);
  assert(erases == 0u && formats == 0u && mounts == 1u);
}

static void non_littlefs(void) {
  reset(8192u);
  memset(image, 0xA5, partition.size);
}

int main(void) {
  reset(8192u);
  mount_result = ESP_OK;
  assert(run().mounted && reads == 0u && formats == 0u && erases == 0u);
  assert(scan_progress_count == 0u && erase_progress_count == 0u);
  assert(scan_reports == 0u);
  reset(8192u);
  assert(run().mounted && formats == 1u && erases == 0u && mounts == 2u);
  assert(scan_reports == 1u && scan_result == H2_EXTFLASH_BLANK);
  assert(scan_info.scanned_bytes == partition.size &&
         scan_info.non_ff_bytes == 0u && scan_info.zero_bytes == 0u &&
         scan_info.nonblank_blocks == 0u && scan_info.nonblank_ranges == 0u);
  /* Unknown old data and incomplete FAT metadata are reformatted. */
  non_littlefs();
  h2_extflash_mount_call_t call = run();
  assert(call.mounted && call.detected_format == H2_EXTFLASH_NON_LITTLEFS);
  assert(reads == 4u && erases == 1u && formats == 1u && mounts == 2u);
  assert(scan_info.scanned_bytes == partition.size &&
         scan_info.non_ff_bytes == partition.size &&
         scan_info.zero_bytes == 0u && scan_info.nonblank_blocks == 2u &&
         scan_info.nonblank_ranges == 1u);
  assert(scan_info.first_non_ff_offset == 0u &&
         scan_info.last_non_ff_offset == partition.size - 1u &&
         scan_info.ranges[0].offset == 0u &&
         scan_info.ranges[0].length == partition.size);
  reset(8192u);
  image[8191] = 0u;
  assert(run().mounted && erases == 1u && formats == 1u);
  assert(scan_info.non_ff_bytes == 1u && scan_info.zero_bytes == 1u &&
         scan_info.first_non_ff_offset == 8191u &&
         scan_info.last_non_ff_offset == 8191u &&
         scan_info.ranges[0].offset == 4096u &&
         scan_info.ranges[0].length == 4096u);
  /* Adjacent dirty blocks form a range; erased blocks separate ranges. */
  reset(5u * 4096u);
  image[3u] = 0u;
  image[4096u] = 0xA5u;
  image[3u * 4096u + 19u] = 0u;
  assert(run().mounted);
  assert(scan_info.non_ff_bytes == 3u && scan_info.zero_bytes == 2u &&
         scan_info.nonblank_blocks == 3u && scan_info.nonblank_ranges == 2u);
  assert(scan_info.first_non_ff_offset == 3u &&
         scan_info.last_non_ff_offset == 3u * 4096u + 19u);
  assert(scan_info.ranges[0].offset == 0u &&
         scan_info.ranges[0].length == 8192u &&
         scan_info.ranges[1].offset == 3u * 4096u &&
         scan_info.ranges[1].length == 4096u);
  /* Keep counting fragmented media after the bounded range list is full. */
  reset((2u * H2_EXTFLASH_SCAN_RANGE_LIMIT + 1u) * 4096u);
  for (size_t i = 0u; i <= H2_EXTFLASH_SCAN_RANGE_LIMIT; ++i) {
    image[2u * i * 4096u] = 0u;
  }
  assert(run().mounted);
  assert(scan_info.nonblank_ranges == H2_EXTFLASH_SCAN_RANGE_LIMIT + 1u &&
         scan_info.non_ff_bytes == H2_EXTFLASH_SCAN_RANGE_LIMIT + 1u);
  assert(scan_info.ranges[H2_EXTFLASH_SCAN_RANGE_LIMIT - 1u].offset ==
         2u * (H2_EXTFLASH_SCAN_RANGE_LIMIT - 1u) * 4096u);
  non_littlefs();
  memcpy(image + 82u, "FAT32   ", 8u);
  assert(run().mounted && erases == 1u && formats == 1u);
  /* Preserve markers at the start, across every chunk split, and at EOF. */
  const size_t offsets[] = {0u,    8u,    4089u, 4090u, 4091u, 4092u,
                            4093u, 4094u, 4095u, 4096u, 8184u};
  for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
    non_littlefs();
    memcpy(image + offsets[i], "littlefs", 8u);
    preserved();
    assert(scan_result == H2_EXTFLASH_LITTLEFS &&
           scan_info.littlefs_marker_offset == offsets[i] &&
           scan_info.scanned_bytes == offsets[i] + 8u);
    assert(memcmp(image + offsets[i], "littlefs", 8u) == 0);
  }
  /* Overlapping candidate prefix must still find the full marker. */
  non_littlefs();
  memcpy(image + 4090u, "littlittlefs", 12u);
  preserved();
  assert(scan_info.littlefs_marker_offset == 4094u);
  reset(5000u);
  memcpy(image + 4992u, "littlefs", 8u);
  preserved();
  assert(scan_info.littlefs_marker_offset == 4992u &&
         scan_info.scanned_bytes == 5000u &&
         scan_info.ranges[0].offset == 4096u &&
         scan_info.ranges[0].length == 904u);
  for (size_t i = 1u; i <= 4u; ++i) {
    non_littlefs();
    fail_read = i;
    preserved();
    assert(scan_result == H2_EXTFLASH_UNKNOWN && !scan_read_mismatch &&
           scan_info.scanned_bytes == ((i - 1u) / 2u) * 4096u &&
           scan_failure_offset == scan_info.scanned_bytes);
    non_littlefs();
    unstable_read = i;
    preserved();
    assert(scan_result == H2_EXTFLASH_UNKNOWN && scan_read_mismatch &&
           scan_info.scanned_bytes == ((i - 1u) / 2u) * 4096u &&
           scan_failure_offset == scan_info.scanned_bytes);
  }
  reset(8192u);
  mount_result = ESP_ERR_NO_MEM;
  preserved();
  assert(reads == 0u);
  reset(8192u);
  mount_result = ESP_ERR_INVALID_STATE;
  preserved();
  assert(reads == 0u);
  non_littlefs();
  erase_result = ESP_FAIL;
  assert(!run().mounted && erases == 1u && formats == 0u && mounts == 1u);
  non_littlefs();
  format_result = ESP_FAIL;
  assert(!run().mounted && erases == 1u && formats == 1u && mounts == 1u);
  non_littlefs();
  remount_result = ESP_FAIL;
  assert(!run().mounted && erases == 1u && formats == 1u && mounts == 2u);
  /* Progress covers full ranges and the tail; a failed range is not counted. */
  const size_t chunk = 1024u * 1024u;
  reset(2u * chunk + 4096u);
  memset(image, 0xA5, partition.size);
  assert(run().mounted && erases == 3u && formats == 1u && mounts == 2u);
  assert(scan_progress_count == 4u && erase_progress_count == 4u);
  for (size_t i = 0u; i < 3u; ++i) {
    assert(scan_progress[i] == i * chunk);
    assert(erase_progress[i] == i * chunk);
  }
  assert(scan_progress[3] == partition.size &&
         erase_progress[3] == partition.size);
  reset(2u * chunk + 4096u);
  memset(image, 0xA5, partition.size);
  fail_erase = 2u;
  assert(!run().mounted && erases == 2u && formats == 0u && mounts == 1u);
  assert(erase_progress_count == 2u && erase_progress[1] == chunk);
  assert(image[chunk - 1u] == 0xFFu && image[chunk] == 0xA5u);
  reset(8192u);
  memcpy(image + 4096u, "littlefs", 8u);
  preserved();
  mounts = 0u;
  mount_result = ESP_OK;
  assert(run().mounted && memcmp(image + 4096u, "littlefs", 8u) == 0);
  free(image);
  puts("extflash recovery tests passed");
  return 0;
}
