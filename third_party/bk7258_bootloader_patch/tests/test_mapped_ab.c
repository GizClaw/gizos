/* Host model of the patched checker. XIP pointers honor the fake remap registers
 * when they are created, so B can only pass when read from the A window base. */
#include <stdio.h>
#include <string.h>

#include "contract.h"

int bk_mapped_ab_check(const void* ignored, const char* name);

enum {
  LOGICAL_SIZE = 0x50000,
  A_LOGICAL = 0x10000,
  B_LOGICAL = 0x30000,
  LOADER_LOGICAL = 0x20000,
  CP_LOGICAL = 0x10000,
  IMAGE_SIZE = 0x11000,
  HEAD_OFFSET = LOADER_LOGICAL - 4096,
};
#define A_BASE (BK_MAPPED_AB_XIP_BASE + A_LOGICAL)
#define REG_BEGIN 0
#define REG_END 1
#define REG_DELTA 2
#define REG_ENABLE 3
#define REG_CCR 4

/* The same golden bytes are produced by apply.py in tests/test_apply.py. */
static const char kGoldenDescriptor[] =
    "484d41420100000040000000070000000010010000100100002002000030030000200200"
    "001001000080000040000000012615907020000000000000daac6457";

static _Alignas(4) uint8_t flash[LOGICAL_SIZE];
static uint8_t a_record[64], b_record[64];
static unsigned crc_counter;
static int crc_calls_until_error;
static int omit_trial_record;
static int failures;

volatile uint32_t bk_mapped_ab_test_registers[5];

unsigned bk_mapped_ab_test_crc_errors(void) {
  if (crc_calls_until_error > 0 && --crc_calls_until_error == 0) ++crc_counter;
  return crc_counter;
}

const uint8_t* bk_mapped_ab_test_lookup(const char* name) {
  if (!strcmp(name, "appa")) return a_record;
  if (!strcmp(name, "s_app")) return omit_trial_record ? NULL : b_record;
  return NULL;
}

const volatile uint8_t* bk_mapped_ab_test_xip(uint32_t address) {
  const volatile uint32_t* r = bk_mapped_ab_test_registers;
  if ((r[REG_ENABLE] & 1u) && address >= r[REG_BEGIN] && address < r[REG_END])
    address += r[REG_DELTA] - BK_MAPPED_AB_XIP_BASE;
  return flash + (address - BK_MAPPED_AB_XIP_BASE);
}

#define CHECK(condition)                                         \
  do {                                                           \
    if (!(condition)) {                                          \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
      ++failures;                                                \
    }                                                            \
  } while (0)

static void put_le32(uint8_t* p, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (i * 8u));
}

static void reseal_descriptor(void) {
  uint8_t* descriptor = flash + BK_MAPPED_AB_DESCRIPTOR_OFFSET;
  put_le32(descriptor + 60, bk_mapped_ab_crc32(descriptor, 60));
}

static bk_mapped_ab_contract_t* descriptor_view(void) {
  return (bk_mapped_ab_contract_t*)(flash + BK_MAPPED_AB_DESCRIPTOR_OFFSET);
}

static void reseal_head(uint8_t* image) {
  uint8_t* head = image + HEAD_OFFSET;
  put_le32(head + 92, bk_mapped_ab_crc32(head, 92));
}

static void write_image(uint8_t* image, uint8_t seed) {
  for (uint32_t i = 0; i < IMAGE_SIZE; ++i)
    image[i] = (uint8_t)(i * 31u + seed);
  put_le32(image, 0x28001000u);
  put_le32(image + 4, A_BASE + 0x101u);
  put_le32(image + CP_LOGICAL, 0x28002000u);
  put_le32(image + CP_LOGICAL + 4, A_BASE + CP_LOGICAL + 0x101u);
  uint32_t hash = 0x811c9dc5u;
  for (uint32_t i = 0; i < IMAGE_SIZE; ++i)
    hash = (hash ^ image[i]) * 0x1000193u;
  uint8_t* head = image + HEAD_OFFSET;
  memset(head, 0, 96);
  put_le32(head, 0x004c4252u);
  put_le32(head + 76, bk_mapped_ab_crc32(image, IMAGE_SIZE));
  put_le32(head + 80, hash);
  put_le32(head + 84, IMAGE_SIZE);
  put_le32(head + 88, IMAGE_SIZE);
  reseal_head(image);
}

static unsigned hex_digit(char digit) {
  return digit <= '9' ? (unsigned)(digit - '0') : (unsigned)(digit - 'a') + 10u;
}

/* Digests follow the checker's 32-byte units, so only the size rule can fail. */
static void set_sizes(uint8_t* image, uint32_t size) {
  const uint32_t hashed = (size + 31u) & ~31u;
  uint32_t hash = 0x811c9dc5u;
  for (uint32_t i = 0; i < hashed; ++i) hash = (hash ^ image[i]) * 0x1000193u;
  put_le32(image + HEAD_OFFSET + 76, bk_mapped_ab_crc32(image, hashed));
  put_le32(image + HEAD_OFFSET + 80, hash);
  put_le32(image + HEAD_OFFSET + 84, size);
  put_le32(image + HEAD_OFFSET + 88, size);
  reseal_head(image);
}

static void reset(void) {
  memset(flash, 0xff, sizeof(flash));
  memset((void*)bk_mapped_ab_test_registers, 0,
         sizeof(bk_mapped_ab_test_registers));
  crc_counter = 0;
  crc_calls_until_error = 0;
  omit_trial_record = 0;
  for (unsigned i = 0; i < 64; ++i) {
    flash[BK_MAPPED_AB_DESCRIPTOR_OFFSET + i] =
        (uint8_t)(hex_digit(kGoldenDescriptor[i * 2u]) << 4 |
                  hex_digit(kGoldenDescriptor[i * 2u + 1u]));
  }
  memset(flash + BK_MAPPED_AB_PAYLOAD_OFFSET, 0xaa, 64);
  write_image(flash + A_LOGICAL, 1);
  write_image(flash + B_LOGICAL, 2);
  memset(a_record, 0, sizeof(a_record));
  memset(b_record, 0, sizeof(b_record));
  put_le32(a_record + 52, 0x11000u);
  put_le32(a_record + 56, 0x22000u);
  put_le32(b_record + 52, 0x33000u);
  put_le32(b_record + 56, 0x22000u);
}

static int mapping_enabled(void) {
  return (bk_mapped_ab_test_registers[REG_ENABLE] & 1u) != 0;
}

static void test_valid_slots(void) {
  reset();
  CHECK(bk_mapped_ab_check(NULL, "appa") == 0);
  CHECK(!mapping_enabled());

  reset();
  CHECK(bk_mapped_ab_check(NULL, "s_app") == 0);
  CHECK(mapping_enabled());
  CHECK(bk_mapped_ab_test_registers[REG_BEGIN] == A_BASE);
  CHECK(bk_mapped_ab_test_registers[REG_END] == A_BASE + LOADER_LOGICAL);
  CHECK(bk_mapped_ab_test_registers[REG_DELTA] ==
        BK_MAPPED_AB_XIP_BASE + (B_LOGICAL - A_LOGICAL));
}

static void test_trial_failure_clears_mapping_and_fallback_uses_loader(void) {
  reset();
  flash[B_LOGICAL + 0x1000] ^= 1u;
  CHECK(bk_mapped_ab_check(NULL, "s_app") != 0);
  CHECK(!mapping_enabled());
  CHECK(bk_mapped_ab_check(NULL, "appa") == 0);
  CHECK(!mapping_enabled());
}

static void test_stale_mapping_is_cleared_before_loader_read(void) {
  reset();
  flash[B_LOGICAL + 0x1000] ^= 1u;
  bk_mapped_ab_test_registers[REG_BEGIN] = A_BASE;
  bk_mapped_ab_test_registers[REG_END] = A_BASE + LOADER_LOGICAL;
  bk_mapped_ab_test_registers[REG_DELTA] =
      BK_MAPPED_AB_XIP_BASE + (B_LOGICAL - A_LOGICAL);
  bk_mapped_ab_test_registers[REG_ENABLE] = 1u;
  CHECK(bk_mapped_ab_check(NULL, "appa") == 0);
  CHECK(!mapping_enabled());
}

static void test_mismatched_header_and_payload_are_rejected(void) {
  reset();
  memcpy(flash + A_LOGICAL + HEAD_OFFSET, flash + B_LOGICAL + HEAD_OFFSET, 96);
  CHECK(bk_mapped_ab_check(NULL, "appa") != 0);
}

static void test_rejects_malformed_headers(void) {
  static const uint32_t kSizes[] = {0, IMAGE_SIZE + 1u, CP_LOGICAL,
                                    HEAD_OFFSET + 32u};
  for (size_t i = 0; i < sizeof(kSizes) / sizeof(kSizes[0]); ++i) {
    reset();
    set_sizes(flash + B_LOGICAL, kSizes[i]);
    CHECK(bk_mapped_ab_check(NULL, "s_app") != 0);
    CHECK(!mapping_enabled());
  }
  /* A size at the limit passes, proving the cases above reach their rule. */
  reset();
  set_sizes(flash + B_LOGICAL, HEAD_OFFSET);
  CHECK(bk_mapped_ab_check(NULL, "s_app") == 0);
  reset();
  put_le32(flash + B_LOGICAL + HEAD_OFFSET + 88, IMAGE_SIZE - 32u);
  reseal_head(flash + B_LOGICAL);
  CHECK(bk_mapped_ab_check(NULL, "s_app") != 0);
  reset();
  put_le32(flash + B_LOGICAL + HEAD_OFFSET, 0);
  reseal_head(flash + B_LOGICAL);
  CHECK(bk_mapped_ab_check(NULL, "s_app") != 0);
  /* Each digest is checked on its own. */
  for (unsigned offset = 76; offset <= 80; offset += 4) {
    reset();
    flash[B_LOGICAL + HEAD_OFFSET + offset] ^= 1u;
    reseal_head(flash + B_LOGICAL);
    CHECK(bk_mapped_ab_check(NULL, "s_app") != 0);
  }
  for (unsigned offset = 4; offset <= 5; ++offset) {
    reset();
    flash[B_LOGICAL + HEAD_OFFSET + offset] = 1u;
    reseal_head(flash + B_LOGICAL);
    CHECK(bk_mapped_ab_check(NULL, "s_app") != 0);
  }
  /* A byte no other rule reads, so only the header CRC can reject it. */
  reset();
  flash[B_LOGICAL + HEAD_OFFSET + 6] ^= 1u;
  CHECK(bk_mapped_ab_check(NULL, "s_app") != 0);
}

static void test_rejects_bad_vectors(void) {
  static const struct {
    uint32_t offset;
    uint32_t value;
  } kCases[] = {
      {0, 0x20000000u},
      {0, 0x28001004u},
      {4, A_BASE + 0x100u},
      {4, A_BASE + CP_LOGICAL + 0x101u},
      {CP_LOGICAL, 0x280a0008u},
      {CP_LOGICAL + 4u, A_BASE + 0x101u},
      {CP_LOGICAL + 4u, A_BASE + IMAGE_SIZE + 1u},
  };
  for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); ++i) {
    reset();
    /* Rebuilding the digests proves the vector rule, not the digest rule. */
    uint8_t* image = flash + B_LOGICAL;
    put_le32(image + kCases[i].offset, kCases[i].value);
    uint32_t hash = 0x811c9dc5u;
    for (uint32_t j = 0; j < IMAGE_SIZE; ++j)
      hash = (hash ^ image[j]) * 0x1000193u;
    put_le32(image + HEAD_OFFSET + 76, bk_mapped_ab_crc32(image, IMAGE_SIZE));
    put_le32(image + HEAD_OFFSET + 80, hash);
    reseal_head(image);
    CHECK(bk_mapped_ab_check(NULL, "s_app") != 0);
  }
}

static void test_crc_counter_rejects_changes_and_saturation(void) {
  /* The first read is the baseline; every later read is a detection point. */
  for (int calls = 2; calls < 5; ++calls) {
    reset();
    crc_calls_until_error = calls;
    CHECK(bk_mapped_ab_check(NULL, "s_app") != 0);
    CHECK(!mapping_enabled());
  }
  reset();
  crc_calls_until_error = (IMAGE_SIZE / 32) + 3;
  CHECK(bk_mapped_ab_check(NULL, "appa") != 0);
  reset();
  crc_counter = 0xffu;
  CHECK(bk_mapped_ab_check(NULL, "appa") != 0);
  reset();
  crc_counter = 3u;
  CHECK(bk_mapped_ab_check(NULL, "appa") == 0);
}

static void test_rejects_enabled_caches(void) {
  reset();
  bk_mapped_ab_test_registers[REG_CCR] = 1u << 16;
  CHECK(bk_mapped_ab_check(NULL, "appa") != 0);
  reset();
  bk_mapped_ab_test_registers[REG_CCR] = 1u << 17;
  CHECK(bk_mapped_ab_check(NULL, "s_app") != 0);
  CHECK(!mapping_enabled());
}

static void test_rejects_descriptor_and_partition_mismatch(void) {
  reset();
  flash[BK_MAPPED_AB_DESCRIPTOR_OFFSET] ^= 1u;
  CHECK(bk_mapped_ab_check(NULL, "appa") != 0);

  reset();
  flash[BK_MAPPED_AB_DESCRIPTOR_OFFSET + 20] ^= 1u;
  CHECK(bk_mapped_ab_check(NULL, "appa") != 0);

  reset();
  flash[BK_MAPPED_AB_PAYLOAD_OFFSET] ^= 1u;
  CHECK(bk_mapped_ab_check(NULL, "appa") != 0);

  reset();
  put_le32(a_record + 52, 0x22000u);
  CHECK(bk_mapped_ab_check(NULL, "appa") != 0);

  reset();
  put_le32(a_record + 56, 0x11000u);
  CHECK(bk_mapped_ab_check(NULL, "appa") != 0);

  reset();
  put_le32(b_record + 52, 0x44000u);
  CHECK(bk_mapped_ab_check(NULL, "s_app") != 0);

  reset();
  put_le32(b_record + 56, 0x11000u);
  CHECK(bk_mapped_ab_check(NULL, "s_app") != 0);

  reset();
  omit_trial_record = 1;
  CHECK(bk_mapped_ab_check(NULL, "appa") != 0);

  reset();
  CHECK(bk_mapped_ab_check(NULL, NULL) != 0);
  CHECK(bk_mapped_ab_check(NULL, "unknown") != 0);
  CHECK(!mapping_enabled());
}

static void test_contract_geometry_limits(void) {
  static const struct {
    size_t offset;
    uint32_t value;
  } kCases[] = {
      {offsetof(bk_mapped_ab_contract_t, version), 2u},
      {offsetof(bk_mapped_ab_contract_t, descriptor_size), 60u},
      {offsetof(bk_mapped_ab_contract_t, flags), 3u},
      {offsetof(bk_mapped_ab_contract_t, boot_size), 0x22000u},
      {offsetof(bk_mapped_ab_contract_t, loader_size), 0x11001u},
      {offsetof(bk_mapped_ab_contract_t, loader_size), 0x33000u},
      {offsetof(bk_mapped_ab_contract_t, trial_offset), 0x22000u},
      {offsetof(bk_mapped_ab_contract_t, trial_offset), 0x33001u},
      {offsetof(bk_mapped_ab_contract_t, trial_offset), 0xff0000u},
      {offsetof(bk_mapped_ab_contract_t, trial_size), 0x1000000u},
      {offsetof(bk_mapped_ab_contract_t, cp_size), 0x22000u},
      {offsetof(bk_mapped_ab_contract_t, cp_size), 0u},
      {offsetof(bk_mapped_ab_contract_t, payload_offset), 0x7f20u},
      {offsetof(bk_mapped_ab_contract_t, payload_offset), 0x8020u},
      {offsetof(bk_mapped_ab_contract_t, trial_size), 0x11000u},
      {offsetof(bk_mapped_ab_contract_t, trial_size), 0x22001u},
      {offsetof(bk_mapped_ab_contract_t, cp_size), 0x11001u},
      {offsetof(bk_mapped_ab_contract_t, payload_size), 0u},
      {offsetof(bk_mapped_ab_contract_t, payload_size), 0x8001u},
      {offsetof(bk_mapped_ab_contract_t, patch_entry), 0x2074u},
      {offsetof(bk_mapped_ab_contract_t, reserved), 1u},
  };
  reset();
  CHECK(bk_mapped_ab_contract_valid(descriptor_view()));
  for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); ++i) {
    reset();
    put_le32(flash + BK_MAPPED_AB_DESCRIPTOR_OFFSET + kCases[i].offset,
             kCases[i].value);
    reseal_descriptor();
    CHECK(!bk_mapped_ab_contract_valid(descriptor_view()));
  }
  /* boot_size must equal loader_offset, so both move to reach its alignment. */
  reset();
  put_le32(flash + BK_MAPPED_AB_DESCRIPTOR_OFFSET +
               offsetof(bk_mapped_ab_contract_t, boot_size),
           0x11001u);
  put_le32(flash + BK_MAPPED_AB_DESCRIPTOR_OFFSET +
               offsetof(bk_mapped_ab_contract_t, loader_offset),
           0x11001u);
  reseal_descriptor();
  CHECK(!bk_mapped_ab_contract_valid(descriptor_view()));
  CHECK(!bk_mapped_ab_contract_valid(NULL));
}

int main(void) {
  test_valid_slots();
  test_trial_failure_clears_mapping_and_fallback_uses_loader();
  test_stale_mapping_is_cleared_before_loader_read();
  test_mismatched_header_and_payload_are_rejected();
  test_rejects_malformed_headers();
  test_rejects_bad_vectors();
  test_crc_counter_rejects_changes_and_saturation();
  test_rejects_enabled_caches();
  test_rejects_descriptor_and_partition_mismatch();
  test_contract_geometry_limits();
  if (failures) fprintf(stderr, "%d mapped AB checks failed\n", failures);
  return failures != 0;
}
