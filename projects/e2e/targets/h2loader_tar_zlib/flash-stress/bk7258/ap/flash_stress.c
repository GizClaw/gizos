#include "flash_stress.h"

#include <bk_private/bk_init.h>
#include <driver/flash.h>
#include <driver/flash_partition.h>
#include <os/os.h>
#include <sdkconfig.h>
#include <soc/reg_base.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "h2_bk_target_task_policy.h"

#if !CONFIG_SOC_SMP || !CONFIG_FLASH_CP_AP_DIRECT_ACCESS
#error "Flash stress requires the native AP SMP/direct Flash path"
#endif
#ifndef CONFIG_FLASH_STRESS_PARTITION_OFFSET
#error "Select a board layout with an App-owned flash_stress partition"
#endif

enum { SECTOR = 4096, CHUNK = 256, ROUNDS = 512 };
_Static_assert(ATOMIC_INT_LOCK_FREE == 2,
               "Progress must use native SRAM atomics");
static const uint32_t scratch = CONFIG_FLASH_STRESS_PARTITION_OFFSET;
static _Atomic uint32_t go, stop, done[2], count[2], stage[2], core[2];
static _Atomic int error[2];
static uint8_t write_buffer[CHUNK], verify_buffer[CHUNK], read_buffer[CHUNK];

/* Static progress lives in internal SRAM. Native word loads/stores avoid a
 * PAL/SDK lock that could serialize the Flash driver's concurrent calls. */
static void publish(_Atomic uint32_t* word, uint32_t value) {
  atomic_store_explicit(word, value, memory_order_release);
}

static uint8_t pattern(uint32_t round, uint32_t offset) {
  return (uint8_t)((round * 73u) ^ (offset * 29u) ^ (offset >> 8u) ^ 0xa5u);
}

/* A bounded direct UART receipt remains usable without the async-log task.
 * Only the controller writes records; workers publish SRAM progress only. */
static void byte(uint8_t value) {
  volatile uint32_t* uart = (volatile uint32_t*)SOC_UART1_REG_BASE;
  for (unsigned attempts = 0; attempts < 200000u; ++attempts) {
    const uint32_t status = uart[6];
    if (!(status & (1u << 16u)) && (status & (1u << 20u))) {
      uart[7] = value;
      return;
    }
  }
}

static void text(const char* value) {
  while (*value) byte((uint8_t)*value++);
}

static void hex(uint32_t value) {
  for (unsigned i = 0; i < 8u; ++i) {
    const unsigned digit = (value >> (28u - 4u * i)) & 15u;
    byte((uint8_t)(digit + (digit < 10u ? '0' : 'a' - 10)));
  }
}

static void record(const char* phase, int rc) {
  text("\r\nH2_FLASH_STRESS phase=");
  text(phase);
  text(" rc_hex=");
  hex((uint32_t)rc);
  text(" writer_rounds=");
  hex(count[0]);
  text(" reader_rounds=");
  hex(count[1]);
  text(" writer_stage=");
  hex(stage[0]);
  text(" reader_stage=");
  hex(stage[1]);
  text(" cores=");
  hex(core[0]);
  byte(',');
  hex(core[1]);
  text("\r\n");
}

static int read_pattern(uint32_t address, uint32_t round, uint8_t* buffer) {
  for (uint32_t offset = 0; offset < SECTOR; offset += CHUNK) {
    int rc = bk_flash_read_bytes(address + offset, buffer, CHUNK);
    if (rc != BK_OK) return rc;
    for (uint32_t i = 0; i < CHUNK; ++i)
      if (buffer[i] != pattern(round, offset + i)) return -1001;
  }
  return BK_OK;
}

static int write_pattern(uint32_t address, uint32_t round) {
  int rc = bk_flash_erase_sector(address);
  if (rc != BK_OK) return rc;
  for (uint32_t offset = 0; offset < SECTOR; offset += CHUNK) {
    for (uint32_t i = 0; i < CHUNK; ++i)
      write_buffer[i] = pattern(round, offset + i);
    rc = bk_flash_write_bytes(address + offset, write_buffer, CHUNK);
    if (rc != BK_OK) return rc;
  }
  return BK_OK;
}

static int verify_erased(void) {
  for (uint32_t offset = 0; offset < 2u * SECTOR; offset += CHUNK) {
    int rc = bk_flash_read_bytes(scratch + offset, verify_buffer, CHUNK);
    if (rc) return rc;
    for (unsigned i = 0; i < CHUNK; ++i)
      if (verify_buffer[i] != 0xffu) return -1006;
  }
  return BK_OK;
}

static void worker(void* argument) {
  const unsigned index = (unsigned)(uintptr_t)argument;
  publish(&core[index], (uint32_t)rtos_get_core_id());
  while (!go) rtos_delay_milliseconds(1);
  __asm volatile("dmb sy" ::: "memory");
  if (core[index] != index + 1u) error[index] = -1002;
  if (index == 0u) {
    for (uint32_t round = 1; round <= ROUNDS && !stop && !error[0]; ++round) {
      publish(&stage[0], 1u);
      error[0] = write_pattern(scratch, round);
      if (error[0]) break;
      publish(&stage[0], 2u);
      error[0] = read_pattern(scratch, round, verify_buffer);
      if (!error[0]) publish(&count[0], round);
    }
  } else {
    while (!stop && !error[1] && !done[0]) {
      publish(&stage[1], 3u);
      error[1] = read_pattern(scratch + SECTOR, 0u, read_buffer);
      if (!error[1]) publish(&count[1], count[1] + 1u);
    }
  }
  if (error[index]) publish(&stop, 1u);
  publish(&stage[index], 4u);
  publish(&done[index], 1u);
  /* Keep the two static workers parked; task deletion is outside this probe. */
  for (;;) rtos_delay_milliseconds(1000);
}

static int validate_region(void) {
  const bk_logic_partition_t* region =
      bk_flash_partition_get_info(BK_PARTITION_FLASH_STRESS);
  const bk_logic_partition_t* app =
      bk_flash_partition_get_info(BK_PARTITION_APPLICATION1);
  if (!region || !app || !region->partition_description ||
      strcmp(region->partition_description, "flash_stress") ||
      region->partition_start_addr != scratch || (scratch % SECTOR) ||
      region->partition_length < 2u * SECTOR ||
      app->partition_start_addr + app->partition_length != scratch ||
      scratch + region->partition_length !=
          CONFIG_OTA_FINA_EXECUTIVE_PARTITION_OFFSET)
    return -1003;
  return BK_OK;
}

void h2_bk_flash_stress_run(void) {
  int rc = validate_region();
  record("region", rc);
  if (rc) return;
  /* Sequential control uses the same real SDK calls before cross-core load. */
  rc = write_pattern(scratch, 0u);
  if (!rc) rc = read_pattern(scratch, 0u, verify_buffer);
  if (!rc) rc = write_pattern(scratch + SECTOR, 0u);
  if (!rc) rc = read_pattern(scratch + SECTOR, 0u, read_buffer);
  record("sequential", rc);
  if (rc) return;
  beken_thread_t workers[2] = {0};
  rc = rtos_core0_create_thread(&workers[0], 7, "flash_writer", worker, 4096,
                                (void*)0);
  if (!rc)
    rc = rtos_core1_create_thread(&workers[1], 7, "flash_reader", worker, 4096,
                                  (void*)1);
  if (rc) {
    publish(&stop, 1u);
    publish(&go, 1u);
    record("thread_error", rc);
    return;
  }
  for (unsigned wait = 0; (!core[0] || !core[1]) && wait < 5000u; ++wait)
    rtos_delay_milliseconds(1);
  if (core[0] != 1u || core[1] != 2u) {
    publish(&stop, 1u);
    publish(&go, 1u);
    record("core_affinity_error", -1002);
    return;
  }
  record("concurrent_begin", 0);
  const uint32_t started = rtos_get_time();
  publish(&go, 1u);
  while (!done[0] || !done[1]) {
    rtos_delay_milliseconds(1000);
    record("progress", error[0] ? error[0] : error[1]);
    if ((uint32_t)(rtos_get_time() - started) >= 120000u) {
      publish(&stop, 1u);
      record("deadline", -1004);
      /* A blocked SDK call still owns its static buffers. Never erase while
       * either worker can be using the test region. */
      return;
    }
  }
  __asm volatile("dmb sy" ::: "memory");
  rc = error[0] ? error[0] : error[1];
  if (!rc && (count[0] != ROUNDS || count[1] == 0u)) rc = -1005;
  int cleanup = bk_flash_erase_sector(scratch);
  if (!cleanup) cleanup = bk_flash_erase_sector(scratch + SECTOR);
  if (!cleanup) cleanup = verify_erased();
  record("cleanup", cleanup);
  record(!rc && !cleanup ? "pass" : "fail", rc ? rc : cleanup);
}

static void controller(void* argument) {
  (void)argument;
  int rc = h2_bk_flash_stress_management_start();
  record("management_ready", rc);
  if (!rc) {
    /* Leave a managed status/reboot window before deliberately stressing IO. */
    rtos_delay_milliseconds(10000);
    h2_bk_flash_stress_run();
  }
  for (;;) rtos_delay_milliseconds(1000);
}

int main(void) {
  if (h2_bk_target_task_policy_install() != 0) return -1;
  bk_init();
  record("boot", 0);
  beken_thread_t thread;
  return rtos_core0_create_thread(&thread, 6, "flash_control", controller,
                                  16384, NULL);
}
