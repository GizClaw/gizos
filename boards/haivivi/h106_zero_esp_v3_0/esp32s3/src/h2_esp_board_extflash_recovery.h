#ifndef H2_ESP_BOARD_EXTFLASH_RECOVERY_H
#define H2_ESP_BOARD_EXTFLASH_RECOVERY_H

#include "esp_partition.h"
#include "esp_littlefs.h"
#include <stdint.h>
#include <string.h>

#define H2_EXTFLASH_SCAN_BYTES 4096u
#define H2_EXTFLASH_PROGRESS_BYTES (1024u * 1024u)
#define H2_EXTFLASH_SCAN_RANGE_LIMIT 8u

typedef void (*h2_extflash_progress_cb_t)(const char* stage, size_t completed,
                                          size_t total);

static void extflash_report_progress(h2_extflash_progress_cb_t progress,
                                     const char* stage, size_t completed,
                                     size_t total) {
  if (progress != NULL) {
    progress(stage, completed, total);
  }
}

/* Private board boot policy: preserve LittleFS evidence and read failures;
 * reinitialize readable media without a LittleFS marker after mount failure. */
typedef enum {
  H2_EXTFLASH_UNKNOWN,
  H2_EXTFLASH_BLANK,
  H2_EXTFLASH_NON_LITTLEFS,
  H2_EXTFLASH_LITTLEFS,
} h2_extflash_format_t;

typedef struct {
  size_t offset;
  size_t length;
} h2_extflash_scan_range_t;

typedef struct {
  size_t scanned_bytes;
  size_t non_ff_bytes;
  size_t zero_bytes;
  size_t first_non_ff_offset;
  size_t last_non_ff_offset;
  size_t littlefs_marker_offset;
  size_t nonblank_blocks;
  size_t nonblank_ranges;
  h2_extflash_scan_range_t ranges[H2_EXTFLASH_SCAN_RANGE_LIMIT];
} h2_extflash_scan_info_t;

typedef struct {
  const esp_partition_t* partition;
  uint8_t* scratch; /* Two scan buffers, owned by safe I/O. */
  esp_err_t read_error;
  size_t failure_offset;
  h2_extflash_progress_cb_t progress;
  h2_extflash_scan_info_t scan;
  int previous_chunk_nonblank;
  int read_mismatch;
} h2_extflash_probe_t;

/* Reports synchronously before recovery; the probe is borrowed for this call.
 */
typedef void (*h2_extflash_scan_report_cb_t)(h2_extflash_format_t format,
                                             const h2_extflash_probe_t* probe);

static void extflash_record_chunk(h2_extflash_probe_t* probe, size_t offset,
                                  size_t length, int nonblank) {
  h2_extflash_scan_info_t* scan = &probe->scan;
  scan->scanned_bytes = offset + length;
  if (!nonblank) {
    probe->previous_chunk_nonblank = 0;
    return;
  }
  ++scan->nonblank_blocks;
  if (!probe->previous_chunk_nonblank) {
    ++scan->nonblank_ranges;
    if (scan->nonblank_ranges <= H2_EXTFLASH_SCAN_RANGE_LIMIT) {
      scan->ranges[scan->nonblank_ranges - 1u] =
          (h2_extflash_scan_range_t){.offset = offset, .length = length};
    }
  } else if (scan->nonblank_ranges <= H2_EXTFLASH_SCAN_RANGE_LIMIT) {
    scan->ranges[scan->nonblank_ranges - 1u].length += length;
  }
  probe->previous_chunk_nonblank = 1;
}

/* A successful SDK read alone is not sufficient evidence for erasure. */
static int extflash_read_stable(h2_extflash_probe_t* probe, size_t offset,
                                size_t length) {
  if (length > H2_EXTFLASH_SCAN_BYTES || offset > probe->partition->size ||
      length > probe->partition->size - offset) {
    probe->read_error = ESP_ERR_INVALID_ARG;
    probe->failure_offset = offset;
    return 0;
  }
  probe->read_error =
      esp_partition_read(probe->partition, offset, probe->scratch, length);
  if (probe->read_error == ESP_OK) {
    probe->read_error =
        esp_partition_read(probe->partition, offset,
                           probe->scratch + H2_EXTFLASH_SCAN_BYTES, length);
  }
  if (probe->read_error == ESP_OK &&
      memcmp(probe->scratch, probe->scratch + H2_EXTFLASH_SCAN_BYTES, length) !=
          0) {
    probe->read_error = ESP_FAIL;
    probe->read_mismatch = 1;
  }
  if (probe->read_error != ESP_OK) {
    probe->failure_offset = offset;
    return 0;
  }
  return 1;
}

static h2_extflash_format_t extflash_probe_format(h2_extflash_probe_t* probe) {
  int blank = 1;
  size_t matched = 0u;
  static const size_t prefix[8] = {0u, 0u, 0u, 0u, 1u, 0u, 0u, 0u};
  memset(&probe->scan, 0, sizeof(probe->scan));
  probe->previous_chunk_nonblank = 0;
  probe->read_mismatch = 0;
  extflash_report_progress(probe->progress, "scan", 0u, probe->partition->size);
  /* Inspect the entire partition, including chunk boundaries and the tail.
   * Any LittleFS marker vetoes destructive recovery. */
  for (size_t offset = 0u; offset < probe->partition->size;) {
    size_t length = probe->partition->size - offset;
    if (length > H2_EXTFLASH_SCAN_BYTES) {
      length = H2_EXTFLASH_SCAN_BYTES;
    }
    if (!extflash_read_stable(probe, offset, length)) {
      return H2_EXTFLASH_UNKNOWN;
    }
    int nonblank = 0;
    for (size_t i = 0u; i < length; ++i) {
      uint8_t byte = probe->scratch[i];
      if (byte != 0xFFu) {
        blank = 0;
        nonblank = 1;
        if (probe->scan.non_ff_bytes == 0u) {
          probe->scan.first_non_ff_offset = offset + i;
        }
        ++probe->scan.non_ff_bytes;
        probe->scan.last_non_ff_offset = offset + i;
        if (byte == 0u) {
          ++probe->scan.zero_bytes;
        }
      }
      while (matched > 0u && byte != (uint8_t)"littlefs"[matched]) {
        matched = prefix[matched - 1u];
      }
      if (byte == (uint8_t)"littlefs"[matched]) {
        if (++matched == 8u) {
          probe->scan.littlefs_marker_offset = offset + i + 1u - 8u;
          extflash_record_chunk(probe, offset, i + 1u, nonblank);
          extflash_report_progress(probe->progress, "scan", offset + i + 1u,
                                   probe->partition->size);
          return H2_EXTFLASH_LITTLEFS;
        }
      }
    }
    extflash_record_chunk(probe, offset, length, nonblank);
    offset += length;
    if (offset % H2_EXTFLASH_PROGRESS_BYTES == 0u ||
        offset == probe->partition->size) {
      extflash_report_progress(probe->progress, "scan", offset,
                               probe->partition->size);
    }
  }
  return blank ? H2_EXTFLASH_BLANK : H2_EXTFLASH_NON_LITTLEFS;
}

/* Mount failure alone never authorizes erasure. */
typedef struct h2_extflash_mount_call {
  const esp_partition_t* partition;
  const char* base_path;
  uint8_t* scratch;
  size_t scan_failure_offset;
  esp_err_t mount_error;
  esp_err_t scan_error;
  esp_err_t format_error;
  esp_err_t remount_error;
  h2_extflash_format_t detected_format;
  esp_err_t erase_error;
  int recovery_started;
  int provisioned;
  int mounted;
  h2_extflash_progress_cb_t progress;
  h2_extflash_scan_report_cb_t scan_report;
} h2_extflash_mount_call_t;

static void IRAM_ATTR mount_safe_callback(void* context) {
  h2_extflash_mount_call_t* call = (h2_extflash_mount_call_t*)context;
  const esp_vfs_littlefs_conf_t config = {
      .base_path = call->base_path,
      .partition = call->partition,
      .format_if_mount_failed = false,
      .dont_mount = false,
  };
  call->mount_error = esp_vfs_littlefs_register(&config);
  if (call->mount_error == ESP_OK) {
    call->mounted = 1;
    return;
  }

  if (call->mount_error != ESP_FAIL) {
    return;
  }
  h2_extflash_probe_t probe = {
      .partition = call->partition,
      .scratch = call->scratch,
      .read_error = ESP_OK,
      .progress = call->progress,
  };
  call->detected_format = extflash_probe_format(&probe);
  call->scan_error = probe.read_error;
  call->scan_failure_offset = probe.failure_offset;
  if (call->scan_report != NULL) {
    call->scan_report(call->detected_format, &probe);
  }
  if (call->detected_format == H2_EXTFLASH_UNKNOWN ||
      call->detected_format == H2_EXTFLASH_LITTLEFS) {
    return;
  }
  call->recovery_started = 1;
  call->erase_error = ESP_OK;
  if (call->detected_format == H2_EXTFLASH_NON_LITTLEFS) {
    extflash_report_progress(call->progress, "erase", 0u,
                             call->partition->size);
    /* Each aligned range finishes before its progress is published. */
    for (size_t offset = 0u; offset < call->partition->size;) {
      size_t length = call->partition->size - offset;
      if (length > H2_EXTFLASH_PROGRESS_BYTES) {
        length = H2_EXTFLASH_PROGRESS_BYTES;
      }
      call->erase_error =
          esp_partition_erase_range(call->partition, offset, length);
      if (call->erase_error != ESP_OK) {
        return;
      }
      offset += length;
      extflash_report_progress(call->progress, "erase", offset,
                               call->partition->size);
    }
  }
  call->format_error = esp_littlefs_format_partition(call->partition);
  if (call->format_error != ESP_OK) {
    return;
  }
  call->remount_error = esp_vfs_littlefs_register(&config);
  if (call->remount_error == ESP_OK) {
    call->provisioned = 1;
    call->mounted = 1;
  }
}

#endif
