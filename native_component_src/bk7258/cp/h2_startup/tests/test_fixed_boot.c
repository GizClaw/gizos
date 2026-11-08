#include "fixed_boot_test_sdk.h"
#include "flash_access.h"
#include "layout.h"
#include <assert.h>
#include <stddef.h>
#include <string.h>

#define LOADER_OFFSET 0x11000u
#define LOADER_CP (1156u * 1024u)
#define LOADER_SIZE (2380u * 1024u)
#define APP_OFFSET (LOADER_OFFSET + LOADER_SIZE)
#define APP_SIZE (5100u * 1024u)
#define DATA_TAIL H2_FIXED_WINDOW_ALIGN
#define FLAGS_OFFSET 0x75f000u
#define CONTROL 0x77f000u

static bk_logic_partition_t cp, ap, other, flags, control;
static h2_fixed_boot_request_t request;
static uint32_t vector[2];
static unsigned reads, writes, vector_reads, stopped, irq_disabled, entered;
static int missing_flags, fail_read, fail_write, fail_vector, fail_unlock;
static flash_protect_type_t protection;
h2_bk_test_systick_t h2_bk_test_systick;
h2_bk_test_nvic_t h2_bk_test_nvic;
h2_bk_test_scb_t h2_bk_test_scb;

const bk_logic_partition_t* bk_flash_partition_get_info(bk_partition_t id) {
  switch (id) {
    case BK_PARTITION_APPLICATION:
      return &cp;
    case BK_PARTITION_APPLICATION1:
      return &ap;
    case BK_PARTITION_S_APP:
      return &other;
    case BK_PARTITION_OTA_FINA_EXECUTIVE:
      return missing_flags ? NULL : &flags;
    case BK_PARTITION_H2_BOOT_REQUEST:
      return &control;
    default:
      return NULL;
  }
}
int bk_flash_read_bytes(uint32_t address, uint8_t* out, uint32_t size) {
  assert(address == CONTROL && size == sizeof(request));
  ++reads;
  if (fail_read) return -1;
  memcpy(out, &request, size);
  return BK_OK;
}
int bk_flash_write_bytes(uint32_t address, const uint8_t* in, uint32_t size) {
  assert(address == CONTROL && size == sizeof(request.magic));
  assert(protection == FLASH_PROTECT_NONE);
  ++writes;
  if (fail_write) return -1;
  uint32_t magic;
  memcpy(&magic, in, sizeof(magic));
  request.magic &= magic;
  return BK_OK;
}
int bk_flash_set_protect_type(flash_protect_type_t type) {
  if (fail_unlock && type == FLASH_PROTECT_NONE) return -1;
  protection = type;
  return BK_OK;
}
flash_protect_type_t bk_flash_get_protect_type(void) { return protection; }
int h2_bk_flash_cp_vector(uint32_t physical, uint32_t out[2]) {
  assert(physical == APP_OFFSET && request.magic == 0u && writes == 1u);
  assert(protection == FLASH_PROTECT_ALL);
  ++vector_reads;
  if (fail_vector) return -1;
  memcpy(out, vector, sizeof(vector));
  return BK_OK;
}
void stop_cpu1_core(void) { stopped |= 1u; }
void stop_cpu2_core(void) { stopped |= 2u; }
void __disable_irq(void) { irq_disabled = 1u; }
void SCB_DisableDCache(void) {}
void SCB_DisableICache(void) {}
void ARM_MPU_Disable(void) {}
void __DSB(void) {}
void __ISB(void) {}
void h2_bk_test_enter_image(uint32_t msp, uint32_t pc) {
  assert(msp == vector[0] && pc == vector[1]);
  assert(stopped == 3u && irq_disabled && SysTick->CTRL == 0u);
  assert(SCB->VTOR == H2_FIXED_XIP_ADDRESS(APP_OFFSET));
  assert(SCB->ICSR == (SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk));
  for (unsigned i = 0; i < 8; ++i)
    assert(NVIC->ICER[i] == UINT32_MAX && NVIC->ICPR[i] == UINT32_MAX);
  ++entered;
}

static void reset_observations(void) {
  reads = writes = vector_reads = stopped = irq_disabled = entered = 0u;
  memset(&h2_bk_test_systick, 0xff, sizeof(h2_bk_test_systick));
  memset(&h2_bk_test_nvic, 0, sizeof(h2_bk_test_nvic));
  memset(&h2_bk_test_scb, 0, sizeof(h2_bk_test_scb));
}
static void fixture(void) {
  cp = (bk_logic_partition_t){LOADER_OFFSET, LOADER_CP};
  ap = (bk_logic_partition_t){LOADER_OFFSET + LOADER_CP,
                              LOADER_SIZE - LOADER_CP};
  other = (bk_logic_partition_t){APP_OFFSET, APP_SIZE};
  flags = (bk_logic_partition_t){FLAGS_OFFSET, H2_FIXED_SECTOR_SIZE};
  control = (bk_logic_partition_t){CONTROL, H2_FIXED_SECTOR_SIZE};
  request = (h2_fixed_boot_request_t){H2_FIXED_REQUEST_MAGIC, APP_OFFSET,
                                      APP_SIZE, 0u, H2_FIXED_ERASED};
  request.check = ~(request.magic ^ request.app_offset ^ request.app_size);
  vector[0] = 0x28080000u;
  vector[1] = H2_FIXED_XIP_ADDRESS(APP_OFFSET) + 0x201u;
  protection = FLASH_PROTECT_ALL;
  missing_flags = fail_read = fail_write = fail_vector = fail_unlock = 0;
  reset_observations();
}
static void assert_no_handoff(void) {
  assert(!entered && !stopped && !irq_disabled);
  assert(SysTick->CTRL == UINT32_MAX && SCB->VTOR == 0u);
}

static void test_trial_and_confirmation(void) {
  fixture();
  h2_bk_cp_try_fixed_app();
  assert(entered == 1u && writes == 1u && request.magic == 0u);
  reset_observations();
  h2_bk_cp_try_fixed_app();
  assert_no_handoff();
  assert(!writes && !vector_reads);
  request.confirmed = 0u;
  h2_bk_cp_try_fixed_app();
  assert(entered == 1u && writes == 1u);

  /* An App code-size record cannot replace the Loader's full-slot record. */
  fixture();
  request.app_size -= DATA_TAIL;
  request.check = ~(request.magic ^ request.app_offset ^ request.app_size);
  h2_bk_cp_try_fixed_app();
  assert_no_handoff();
  assert(reads == 1u && !writes && !vector_reads);
}
static void test_rejects_execution_flags_and_app_table(void) {
  const bk_logic_partition_t invalid_flags[] = {
      {FLAGS_OFFSET - DATA_TAIL, H2_FIXED_SECTOR_SIZE},
      {FLAGS_OFFSET + H2_FIXED_SECTOR_SIZE, H2_FIXED_SECTOR_SIZE},
      {FLAGS_OFFSET + 1u, H2_FIXED_SECTOR_SIZE},
      {FLAGS_OFFSET, CONTROL - FLAGS_OFFSET + H2_FIXED_SECTOR_SIZE},
      {FLAGS_OFFSET, H2_FIXED_SECTOR_SIZE - 1u},
      {FLAGS_OFFSET, 0u},
  };
  for (unsigned i = 0; i < sizeof(invalid_flags) / sizeof(invalid_flags[0]);
       ++i) {
    fixture();
    flags = invalid_flags[i];
    h2_bk_cp_try_fixed_app();
    assert_no_handoff();
    assert(!reads && !writes && !vector_reads);
  }
  fixture();
  missing_flags = 1;
  h2_bk_cp_try_fixed_app();
  assert_no_handoff();
  assert(!reads && !writes);
  fixture();
  other.partition_length -= DATA_TAIL;
  h2_bk_cp_try_fixed_app();
  assert_no_handoff();
  assert(!reads && !writes);

  fixture();
  cp = (bk_logic_partition_t){APP_OFFSET, LOADER_CP};
  ap = (bk_logic_partition_t){APP_OFFSET + LOADER_CP,
                              APP_SIZE - LOADER_CP - DATA_TAIL};
  other = (bk_logic_partition_t){LOADER_OFFSET, LOADER_SIZE};
  h2_bk_cp_try_fixed_app();
  assert_no_handoff();
  assert(!reads && !writes && !vector_reads);
}
static void test_failures_do_not_enter_app(void) {
  fixture();
  fail_read = 1;
  h2_bk_cp_try_fixed_app();
  assert_no_handoff();
  assert(!writes && !vector_reads);
  fixture();
  fail_unlock = 1;
  h2_bk_cp_try_fixed_app();
  assert_no_handoff();
  assert(!writes && !vector_reads && protection == FLASH_PROTECT_ALL);
  fixture();
  fail_write = 1;
  h2_bk_cp_try_fixed_app();
  assert_no_handoff();
  assert(writes == 1u && !vector_reads && protection == FLASH_PROTECT_ALL);
  fixture();
  fail_vector = 1;
  h2_bk_cp_try_fixed_app();
  assert_no_handoff();
  assert(writes == 1u && vector_reads == 1u && request.magic == 0u);
  fixture();
  vector[1] = H2_FIXED_XIP_ADDRESS(APP_OFFSET + APP_SIZE) + 1u;
  h2_bk_cp_try_fixed_app();
  assert_no_handoff();
  assert(writes == 1u && request.magic == 0u);
}

int main(void) {
  test_trial_and_confirmation();
  test_rejects_execution_flags_and_app_table();
  test_failures_do_not_enter_app();
  return 0;
}
