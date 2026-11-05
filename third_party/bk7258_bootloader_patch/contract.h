#ifndef BK7258_MAPPED_AB_CONTRACT_H
#define BK7258_MAPPED_AB_CONTRACT_H

#include <stddef.h>
#include <stdint.h>

#define BK_MAPPED_AB_MAGIC 0x42414d48u
#define BK_MAPPED_AB_VERSION 1u
#define BK_MAPPED_AB_FLAGS 7u
#define BK_MAPPED_AB_DESCRIPTOR_OFFSET 0x7f00u
#define BK_MAPPED_AB_PAYLOAD_OFFSET 0x8000u
#define BK_MAPPED_AB_PATCH_ENTRY 0x2070u
#define BK_MAPPED_AB_XIP_BASE 0x02000000u
#define BK_MAPPED_AB_PHYSICAL_LIMIT 0x01000000u

/* Stored little-endian in the logical (CRC-stripped) bootloader window.
 * A compatibility/integrity descriptor, not an authentication credential.
 * The final word is CRC32 over the preceding 60 bytes. */
typedef struct bk_mapped_ab_contract {
  uint32_t magic;
  uint32_t version;
  uint32_t descriptor_size;
  uint32_t flags;
  uint32_t boot_size;
  uint32_t loader_offset;
  uint32_t loader_size;
  uint32_t trial_offset;
  uint32_t trial_size;
  uint32_t cp_size;
  uint32_t payload_offset;
  uint32_t payload_size;
  uint32_t payload_crc32;
  uint32_t patch_entry;
  uint32_t reserved;
  uint32_t descriptor_crc32;
} bk_mapped_ab_contract_t;

_Static_assert(sizeof(bk_mapped_ab_contract_t) == 64u,
               "mapped_ab_v1 descriptor ABI changed");

static inline uint32_t bk_mapped_ab_logical(uint32_t physical) {
  return physical / 34u * 32u;
}

static inline uint32_t bk_mapped_ab_crc_step(uint32_t crc, uint8_t byte) {
  crc ^= byte;
  for (unsigned i = 0; i < 8; ++i)
    crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  return crc;
}

static inline uint32_t bk_mapped_ab_crc32(const void* data, size_t size) {
  const uint8_t* bytes = data;
  uint32_t crc = 0xffffffffu;
  while (size--) crc = bk_mapped_ab_crc_step(crc, *bytes++);
  return ~crc;
}

/* Validate geometry before calculating any mapped pointer or erase range. */
static inline int bk_mapped_ab_contract_valid(
    const bk_mapped_ab_contract_t* c) {
  if (!c || c->magic != BK_MAPPED_AB_MAGIC ||
      c->version != BK_MAPPED_AB_VERSION || c->descriptor_size != sizeof(*c) ||
      c->flags != BK_MAPPED_AB_FLAGS || c->reserved ||
      c->patch_entry != BK_MAPPED_AB_PATCH_ENTRY ||
      c->payload_offset != BK_MAPPED_AB_PAYLOAD_OFFSET || !c->payload_size ||
      c->boot_size != c->loader_offset || c->loader_size > c->trial_size ||
      !c->cp_size || c->cp_size >= c->loader_size)
    return 0;
  const uint32_t alignment = 68u * 1024u;
  if (!c->boot_size || c->boot_size % alignment ||
      !c->loader_size || c->loader_size % alignment ||
      !c->trial_size || c->trial_size % alignment ||
      c->trial_offset % alignment || c->cp_size % alignment ||
      c->loader_offset >= BK_MAPPED_AB_PHYSICAL_LIMIT ||
      c->loader_size > BK_MAPPED_AB_PHYSICAL_LIMIT - c->loader_offset ||
      c->trial_offset >= BK_MAPPED_AB_PHYSICAL_LIMIT ||
      c->trial_size > BK_MAPPED_AB_PHYSICAL_LIMIT - c->trial_offset ||
      c->loader_offset + c->loader_size > c->trial_offset)
    return 0;
  const uint32_t boot_logical = bk_mapped_ab_logical(c->boot_size);
  if (c->payload_offset >= boot_logical ||
      c->payload_size > boot_logical - c->payload_offset ||
      BK_MAPPED_AB_DESCRIPTOR_OFFSET + sizeof(*c) > c->payload_offset ||
      bk_mapped_ab_logical(c->loader_size) <=
          bk_mapped_ab_logical(c->cp_size) + 4096u)
    return 0;
  return bk_mapped_ab_crc32(c, offsetof(bk_mapped_ab_contract_t,
                                      descriptor_crc32)) == c->descriptor_crc32;
}

#endif
