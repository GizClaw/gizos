#include "app_config.h"

#include "asm/system_reset_reason.h"
#include "fs/fs.h"
#include "h2_jieli_ac791n_devkit_partitions.h"
#include "system/includes.h"
#include "utils/fs/sdfile.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define H2_JIELI_COREDUMP_MAGIC UINT32_C(0x52433248) /* H2CR */
#define H2_JIELI_COREDUMP_VERSION UINT32_C(2)
#define H2_JIELI_COREDUMP_COMMITTED UINT32_C(0x54494d43) /* CMIT */
#define H2_JIELI_CRASH_PENDING UINT32_C(0x48535243) /* CRSH */
#define H2_JIELI_RETAINED_LOG_MAGIC UINT32_C(0x474f4c48) /* HLOG */
#define H2_JIELI_RETAINED_LOG_CAPACITY 2048u
#define H2_JIELI_EXCEPTION_REASON UINT32_C(0x80000000)

struct h2_jieli_wl82_boot_marker {
  unsigned int magic0;
  unsigned int magic1;
  unsigned int stage;
  int result;
};

extern volatile struct h2_jieli_wl82_boot_marker h2_jieli_wl82_ram_marker;
extern int printf(const char *format, ...);
extern void put_buf(const uint8_t *buffer, int length);

struct h2_jieli_coredump_record {
  /* The shared H2Loader coredump command requires total size in word zero. */
  uint32_t size;
  uint32_t magic;
  uint32_t version;
  uint32_t sequence;
  uint32_t reset_reason;
  uint32_t boot_stage;
  uint32_t caller;
  uint32_t marker_result;
  uint32_t log_bytes;
  uint32_t log_total;
  uint8_t log[H2_JIELI_RETAINED_LOG_CAPACITY];
  uint32_t checksum;
  uint32_t committed;
};

struct h2_jieli_retained_log {
  uint32_t magic;
  uint32_t head;
  uint32_t total;
  uint8_t data[H2_JIELI_RETAINED_LOG_CAPACITY];
};

static uint32_t coredump_sdfile_addr SEC(.volatile_ram);
static uint32_t coredump_sequence SEC(.h2_retained);
static struct h2_jieli_coredump_record pending_record
    SEC(.h2_retained);
static volatile uint32_t crash_pending SEC(.h2_retained);
static struct h2_jieli_retained_log retained_log SEC(.h2_retained);
static struct h2_jieli_coredump_record replay_record;
static volatile uint8_t suppress_log_capture;

static uint32_t h2_jieli_checksum(
    const struct h2_jieli_coredump_record *record) SEC(.volatile_ram_code);
static int h2_jieli_write_record(
    const struct h2_jieli_coredump_record *record) SEC(.volatile_ram_code);

static uint32_t h2_jieli_checksum(
    const struct h2_jieli_coredump_record *record) {
  const uint8_t *bytes = (const uint8_t *)record;
  uint32_t checksum = UINT32_C(0x811c9dc5);
  const size_t count = offsetof(struct h2_jieli_coredump_record, checksum);
  for (size_t i = 0u; i < count; ++i) {
    checksum ^= bytes[i];
    checksum *= UINT32_C(0x01000193);
  }
  return checksum;
}

static int h2_jieli_record_valid(
    const struct h2_jieli_coredump_record *record) {
  return record->magic == H2_JIELI_COREDUMP_MAGIC &&
         record->version == H2_JIELI_COREDUMP_VERSION &&
         record->size == sizeof(*record) &&
         record->log_bytes <= sizeof(record->log) &&
         record->committed == H2_JIELI_COREDUMP_COMMITTED &&
         record->checksum == h2_jieli_checksum(record);
}

void h2_jieli_wl82_log_byte(char value) SEC(.volatile_ram_code);

void h2_jieli_wl82_log_byte(char value) {
  if (suppress_log_capture) return;
  if (retained_log.magic != H2_JIELI_RETAINED_LOG_MAGIC ||
      retained_log.head >= sizeof(retained_log.data)) {
    retained_log.magic = H2_JIELI_RETAINED_LOG_MAGIC;
    retained_log.head = 0u;
    retained_log.total = 0u;
  }
  retained_log.data[retained_log.head] = (uint8_t)value;
  retained_log.head = (retained_log.head + 1u) % sizeof(retained_log.data);
  ++retained_log.total;
}

static void h2_jieli_build_record(
    struct h2_jieli_coredump_record *record,
    uint32_t reset_reason,
    void *caller) SEC(.volatile_ram_code);

static void h2_jieli_build_record(
    struct h2_jieli_coredump_record *record,
    uint32_t reset_reason,
    void *caller) {
  memset(record, 0, sizeof(*record));
  record->size = sizeof(*record);
  record->magic = H2_JIELI_COREDUMP_MAGIC;
  record->version = H2_JIELI_COREDUMP_VERSION;
  record->sequence = ++coredump_sequence;
  record->reset_reason = reset_reason;
  record->boot_stage = h2_jieli_wl82_ram_marker.stage;
  record->caller = (uint32_t)(uintptr_t)caller;
  record->marker_result = (uint32_t)h2_jieli_wl82_ram_marker.result;
  if (retained_log.magic == H2_JIELI_RETAINED_LOG_MAGIC &&
      retained_log.head < sizeof(retained_log.data)) {
    uint32_t available = retained_log.total < sizeof(retained_log.data)
                             ? retained_log.total
                             : (uint32_t)sizeof(retained_log.data);
    uint32_t start =
        (retained_log.head + sizeof(retained_log.data) - available) %
        sizeof(retained_log.data);
    record->log_bytes = available;
    record->log_total = retained_log.total;
    for (uint32_t i = 0u; i < available; ++i) {
      record->log[i] =
          retained_log.data[(start + i) % sizeof(retained_log.data)];
    }
  }
  record->committed = H2_JIELI_COREDUMP_COMMITTED;
  record->checksum = h2_jieli_checksum(record);
  retained_log.magic = H2_JIELI_RETAINED_LOG_MAGIC;
  retained_log.head = 0u;
  retained_log.total = 0u;
}

/*
 * sclust is the SDK sdfile CPU address, not a raw NOR offset. The reserved-zone
 * API performs the required address translation and is also what JieLi's own
 * exception logger uses while normal filesystem services may be unavailable.
 */
static int h2_jieli_write_record(
    const struct h2_jieli_coredump_record *record) {
  if (coredump_sdfile_addr == 0u) {
    return -1;
  }

  /* This runs from the Loader task after the OS, USB, and SDFILE runtime are
   * online.  Do not copy the exception-context preparation from debug.c here:
   * set_os_init_flag(0) tears down the live OS contract and can strand USB
   * before the erase completes.  Normal SDK users call the reserve-zone API
   * directly from task context. */
  if (sdfile_reserve_zone_erase(
          coredump_sdfile_addr, SDFILE_SECTOR_SIZE, 0) < 0) {
    return -1;
  }
  if (sdfile_reserve_zone_write(
      (void *)record, coredump_sdfile_addr,
      offsetof(struct h2_jieli_coredump_record, committed), 0) !=
      offsetof(struct h2_jieli_coredump_record, committed)) {
    return -1;
  }

  const uint32_t committed = H2_JIELI_COREDUMP_COMMITTED;
  if (sdfile_reserve_zone_write(
      (void *)&committed,
      coredump_sdfile_addr +
          offsetof(struct h2_jieli_coredump_record, committed),
      sizeof(committed), 0) != sizeof(committed)) {
    return -1;
  }
  return 0;
}

void h2_jieli_wl82_assert_reset_hook(void *caller) SEC(.volatile_ram_code);

void h2_jieli_wl82_assert_reset_hook(void *caller) {
  crash_pending = H2_JIELI_CRASH_PENDING;
  h2_jieli_build_record(
      &pending_record, H2_JIELI_EXCEPTION_REASON, caller);
  /* Exception context may have interrupts disabled or another core paused.
   * Persisting through the NOR driver here can therefore deadlock before the
   * reset.  The record lives in non-volatile RAM and coredump_init writes it
   * only after the next boot has restored the normal flash environment. */
}

void h2_jieli_wl82_reset_recovery_hook(uint32_t reset_reason)
    SEC(.volatile_ram_code);

void h2_jieli_wl82_reset_recovery_hook(uint32_t reset_reason) {
  if ((reset_reason & SYS_RST_WDT) == 0u) return;
  crash_pending = H2_JIELI_CRASH_PENDING;
  h2_jieli_build_record(&pending_record, reset_reason, NULL);
}

int h2_jieli_wl82_take_crash_pending(void) {
  const int pending = crash_pending == H2_JIELI_CRASH_PENDING;
  crash_pending = 0u;
  return pending;
}

int h2_jieli_wl82_coredump_flush_pending(void) {
  if (!h2_jieli_record_valid(&pending_record)) return 0;
  if (h2_jieli_write_record(&pending_record) != 0) return -1;
  if (sdfile_reserve_zone_read(
          &replay_record, coredump_sdfile_addr,
          sizeof(replay_record), 0) != sizeof(replay_record) ||
      !h2_jieli_record_valid(&replay_record)) {
    return -1;
  }
  memset(&pending_record, 0, sizeof(pending_record));
  return 1;
}

static int h2_jieli_coredump_init(void) {
  static const char *const paths[] = {
      "mnt/sdfile/EXT_RESERVED/H2CORE",
      "mnt/sdfile/EXT_RESERVED/h2core",
  };

  for (size_t i = 0u; i < sizeof(paths) / sizeof(paths[0]); ++i) {
    FILE *file = fopen(paths[i], "r");
    if (file == NULL) {
      continue;
    }
    struct vfs_attr attrs = {0};
    if (fget_attrs(file, &attrs) == 0 && attrs.sclust != 0u &&
        attrs.fsize >= SDFILE_SECTOR_SIZE) {
      coredump_sdfile_addr = attrs.sclust;
    }
    fclose(file);
    if (coredump_sdfile_addr != 0u) {
      break;
    }
  }

  /* A compact double-bank image can omit reserved-file directory entries.
   * Convert the board layout's physical address into the CPU address expected
   * by sdfile_reserve_zone_* so coredumps do not depend on that metadata. */
  if (coredump_sdfile_addr == 0u) {
    coredump_sdfile_addr =
        sdfile_flash_addr2cpu_addr(H2_JIELI_COREDUMP_ADDRESS);
  }

  /* Do not write Flash from platform_initcall. At this point USB, the Loader
   * command task, and SD runtime are not ready, so a blocked NOR operation
   * would make the only recovery diagnostics disappear. loader_task flushes
   * this retained record after USB and storage are both online. */
  if (coredump_sdfile_addr != 0u &&
      sdfile_reserve_zone_read(
          &replay_record, coredump_sdfile_addr,
          sizeof(replay_record), 0) == sizeof(replay_record) &&
      h2_jieli_record_valid(&replay_record)) {
    suppress_log_capture = 1u;
    printf(
        "H2_JIELI_COREDUMP_REPLAY sequence=%u reset_reason=0x%x "
        "boot_stage=%u caller=0x%x marker_result=%d log_bytes=%u "
        "log_total=%u\r\n",
        (unsigned)replay_record.sequence,
        (unsigned)replay_record.reset_reason,
        (unsigned)replay_record.boot_stage,
        (unsigned)replay_record.caller,
        (int)replay_record.marker_result,
        (unsigned)replay_record.log_bytes,
        (unsigned)replay_record.log_total);
    printf("H2_JIELI_COREDUMP_LOG_BEGIN\r\n");
    if (replay_record.log_bytes != 0u) {
      put_buf(replay_record.log, (int)replay_record.log_bytes);
    }
    printf("\r\nH2_JIELI_COREDUMP_LOG_END\r\n");
    suppress_log_capture = 0u;
  }
  return 0;
}

platform_initcall(h2_jieli_coredump_init);
