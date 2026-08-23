#include "h2_bk3633_sdk_runtime_internal.h"

#include "flash.h"
#include "nvds.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define BK3633_NVDS_ABI_SIZE 0x0400u
#define BK3633_NVDS_ABI_MAGIC_SIZE 4u
#define BK3633_NVDS_ABI_HEADER_SIZE 3u
#define BK3633_NVDS_ABI_STATUS_VALID_MASK 0x01u
#define BK3633_NVDS_ABI_STATUS_LOCKED_MASK 0x02u
#define BK3633_NVDS_ABI_STATUS_ERASED_MASK 0x04u
#define BK3633_NVDS_ABI_STATUS_OK 0x06u
#define BK3633_NVDS_ABI_COMPARE_CHUNK_SIZE 32u
#define BK3633_NVDS_ABI_MAINTENANCE_RESERVE 256u

typedef struct bk3633_nvds_header {
    uint8_t tag;
    uint8_t status;
    uint8_t length;
} bk3633_nvds_header_t;

typedef struct bk3633_nvds_record {
    bk3633_nvds_header_t header;
    uint32_t offset;
    uint32_t next;
} bk3633_nvds_record_t;

static const uint8_t s_nvds_magic[BK3633_NVDS_ABI_MAGIC_SIZE] = {
    'N', 'V', 'D', 'S'
};

_Static_assert(NVDS_PACKED == 1u,
               "BK3633 NVDS requires packed records");
_Static_assert(sizeof(nvds_tag_len_t) == 1u,
               "BK3633 NVDS requires 8-bit lengths");
_Static_assert(sizeof(bk3633_nvds_header_t) == BK3633_NVDS_ABI_HEADER_SIZE,
               "BK3633 NVDS header layout mismatch");

static bool bk3633_nvds_record_valid(const bk3633_nvds_record_t *record) {
    return (record->header.status &
            (BK3633_NVDS_ABI_STATUS_VALID_MASK |
             BK3633_NVDS_ABI_STATUS_ERASED_MASK)) ==
           BK3633_NVDS_ABI_STATUS_ERASED_MASK;
}

static bool bk3633_nvds_record_locked(const bk3633_nvds_record_t *record) {
    return (record->header.status & BK3633_NVDS_ABI_STATUS_LOCKED_MASK) == 0u;
}

static uint8_t bk3633_nvds_read(uint32_t offset, void *data, uint32_t len) {
    if (offset > BK3633_NVDS_ABI_SIZE || len > BK3633_NVDS_ABI_SIZE - offset) {
        return NVDS_CORRUPT;
    }
    return flash_read(0u, flash_env.nvds_def_addr_abs + offset, len,
                      (uint8_t *)data, NULL) == 0u ? NVDS_OK : NVDS_FAIL;
}

static uint8_t bk3633_nvds_write(uint32_t offset,
                                  const void *data,
                                  uint32_t len) {
    if (offset > BK3633_NVDS_ABI_SIZE || len > BK3633_NVDS_ABI_SIZE - offset) {
        return NVDS_CORRUPT;
    }
    return flash_write(0u, flash_env.nvds_def_addr_abs + offset, len,
                       (uint8_t *)(uintptr_t)data, NULL) == 0u
               ? NVDS_OK
               : NVDS_FAIL;
}

static uint8_t bk3633_nvds_walk(uint32_t offset,
                                 bk3633_nvds_record_t *out_record) {
    bk3633_nvds_header_t header;
    uint8_t status;

    if (out_record == NULL ||
        offset > BK3633_NVDS_ABI_SIZE - BK3633_NVDS_ABI_HEADER_SIZE) {
        return NVDS_CORRUPT;
    }
    status = bk3633_nvds_read(offset, &header, sizeof(header));
    if (status != NVDS_OK) {
        return status;
    }
    if (header.tag == 0xffu) {
        return NVDS_TAG_NOT_DEFINED;
    }
    out_record->header = header;
    out_record->offset = offset;
    out_record->next = offset + BK3633_NVDS_ABI_HEADER_SIZE + header.length;
    return out_record->next < BK3633_NVDS_ABI_SIZE ? NVDS_OK : NVDS_CORRUPT;
}

static uint8_t bk3633_nvds_find(uint8_t tag,
                                 bk3633_nvds_record_t *out_record) {
    uint32_t offset = BK3633_NVDS_ABI_MAGIC_SIZE;
    bk3633_nvds_record_t record;
    uint8_t status;

    for (;;) {
        status = bk3633_nvds_walk(offset, &record);
        if (status != NVDS_OK) {
            return status;
        }
        if (record.header.tag == tag && bk3633_nvds_record_valid(&record)) {
            if (out_record != NULL) {
                *out_record = record;
            }
            return NVDS_OK;
        }
        offset = record.next;
    }
}

static uint8_t bk3633_nvds_mark(uint32_t offset, uint8_t status) {
    return bk3633_nvds_write(
        offset + offsetof(bk3633_nvds_header_t, status), &status, 1u);
}

static uint8_t bk3633_nvds_append(uint32_t offset,
                                   uint8_t tag,
                                   uint8_t length,
                                   const uint8_t *data) {
    const bk3633_nvds_header_t header = {
        .tag = tag,
        .status = BK3633_NVDS_ABI_STATUS_OK,
        .length = length,
    };
    uint8_t status;

    if (offset + BK3633_NVDS_ABI_HEADER_SIZE + length >= BK3633_NVDS_ABI_SIZE) {
        return NVDS_NO_SPACE_AVAILABLE;
    }
    status = bk3633_nvds_write(offset + BK3633_NVDS_ABI_HEADER_SIZE,
                                data, length);
    if (status != NVDS_OK) {
        return status;
    }
    return bk3633_nvds_write(offset, &header, sizeof(header));
}

static uint8_t bk3633_nvds_value_equal(const bk3633_nvds_record_t *record,
                                        const uint8_t *data,
                                        uint8_t length,
                                        bool *out_equal) {
    uint8_t chunk[BK3633_NVDS_ABI_COMPARE_CHUNK_SIZE];
    uint32_t copied = 0u;

    *out_equal = false;
    if (record->header.length != length) {
        return NVDS_OK;
    }
    while (copied < length) {
        uint32_t count = (uint32_t)length - copied;
        if (count > sizeof(chunk)) {
            count = sizeof(chunk);
        }
        uint8_t status = bk3633_nvds_read(
            record->offset + BK3633_NVDS_ABI_HEADER_SIZE + copied,
            chunk, count);
        if (status != NVDS_OK) {
            return status;
        }
        if (memcmp(chunk, data + copied, count) != 0) {
            return NVDS_OK;
        }
        copied += count;
    }
    *out_equal = true;
    return NVDS_OK;
}

uint8_t nvds_init(void) {
    uint8_t magic[BK3633_NVDS_ABI_MAGIC_SIZE];

    if (bk3633_nvds_read(0u, magic, sizeof(magic)) == NVDS_OK &&
        memcmp(magic, s_nvds_magic, sizeof(magic)) == 0) {
        return NVDS_OK;
    }
    if (flash_erase(0u, flash_env.nvds_def_addr_abs,
                    BK3633_NVDS_ABI_SIZE, NULL) != 0u) {
        return NVDS_FAIL;
    }
    return bk3633_nvds_write(0u, s_nvds_magic, sizeof(s_nvds_magic));
}

uint8_t nvds_get(uint8_t tag, nvds_tag_len_t *length, uint8_t *data) {
    bk3633_nvds_record_t record;
    uint8_t status;

    if (length == NULL || (data == NULL && *length != 0u)) {
        return NVDS_FAIL;
    }
    status = bk3633_nvds_find(tag, &record);
    /*
     * Legacy H300 firmware treats an unreadable tail as a missing application
     * tag and recreates the value.  Preserve that behavior here: a structural
     * tail error must not leave first-boot provisioning in a retry loop.  A
     * later nvds_put() compacts the valid prefix before appending the value.
     * Flash I/O failures remain hard failures and are never hidden.
     */
    if (status == NVDS_CORRUPT) {
        status = NVDS_TAG_NOT_DEFINED;
    }
    if (status != NVDS_OK) {
        *length = 0u;
        return status;
    }
    if (*length < record.header.length) {
        if (h2_bk3633_sdk_runtime_nvds_oversize_is_missing(
                tag, record.header.length, *length)) {
            *length = 0u;
            return NVDS_TAG_NOT_DEFINED;
        }
        *length = record.header.length;
        return NVDS_LENGTH_OUT_OF_RANGE;
    }
    status = bk3633_nvds_read(
        record.offset + BK3633_NVDS_ABI_HEADER_SIZE,
        data, record.header.length);
    if (status == NVDS_OK) {
        *length = record.header.length;
    }
    return status;
}

uint8_t nvds_del(uint8_t tag) {
    bk3633_nvds_record_t record;
    uint8_t status = bk3633_nvds_find(tag, &record);
    if (status != NVDS_OK) {
        return status;
    }
    if (bk3633_nvds_record_locked(&record)) {
        return NVDS_PARAM_LOCKED;
    }
    return bk3633_nvds_mark(
        record.offset,
        (uint8_t)(record.header.status & ~BK3633_NVDS_ABI_STATUS_ERASED_MASK));
}

uint8_t nvds_lock(uint8_t tag) {
    bk3633_nvds_record_t record;
    uint8_t status = bk3633_nvds_find(tag, &record);
    if (status != NVDS_OK) {
        return status;
    }
    return bk3633_nvds_mark(
        record.offset,
        (uint8_t)(record.header.status & ~BK3633_NVDS_ABI_STATUS_LOCKED_MASK));
}

static uint8_t bk3633_nvds_compact(uint8_t tag,
                                    uint8_t length,
                                    const uint8_t *data,
                                    bool replace_tag) {
    const h2_pal_mem_api_t *mem = h2_bk3633_sdk_runtime_nvds_mem_api();
    uint8_t *scratch = mem == NULL
                           ? NULL
                           : h2_pal_mem_alloc(mem, BK3633_NVDS_ABI_SIZE);
    uint32_t source = BK3633_NVDS_ABI_MAGIC_SIZE;
    uint32_t target = BK3633_NVDS_ABI_MAGIC_SIZE;
    bk3633_nvds_record_t record;
    uint8_t status = scratch == NULL ? NVDS_NO_SPACE_AVAILABLE : NVDS_OK;

    while (status == NVDS_OK) {
        status = bk3633_nvds_walk(source, &record);
        if (status == NVDS_TAG_NOT_DEFINED) {
            status = NVDS_OK;
            break;
        }
        if (status == NVDS_CORRUPT) {
            /* Keep the readable prefix and discard the malformed tail. */
            status = NVDS_OK;
            break;
        }
        if (status != NVDS_OK) {
            break;
        }
        if ((!replace_tag || record.header.tag != tag) &&
            bk3633_nvds_record_valid(&record)) {
            uint32_t size = record.next - record.offset;
            if (target + size + BK3633_NVDS_ABI_HEADER_SIZE >=
                BK3633_NVDS_ABI_SIZE) {
                status = NVDS_NO_SPACE_AVAILABLE;
                break;
            }
            status = bk3633_nvds_read(record.offset, scratch + target, size);
            target += status == NVDS_OK ? size : 0u;
        }
        source = record.next;
    }
    if (status == NVDS_OK && replace_tag &&
        target + BK3633_NVDS_ABI_HEADER_SIZE + length < BK3633_NVDS_ABI_SIZE) {
        const bk3633_nvds_header_t header = {
            .tag = tag,
            .status = BK3633_NVDS_ABI_STATUS_OK,
            .length = length,
        };
        memcpy(scratch + target, &header, sizeof(header));
        memcpy(scratch + target + sizeof(header), data, length);
        target += sizeof(header) + length;
    } else if (status == NVDS_OK && replace_tag) {
        status = NVDS_NO_SPACE_AVAILABLE;
    }
    if (status == NVDS_OK &&
        (flash_erase(0u, flash_env.nvds_def_addr_abs,
                     BK3633_NVDS_ABI_SIZE, NULL) != 0u ||
         bk3633_nvds_write(0u, s_nvds_magic,
                            sizeof(s_nvds_magic)) != NVDS_OK ||
         (target > BK3633_NVDS_ABI_MAGIC_SIZE &&
          bk3633_nvds_write(BK3633_NVDS_ABI_MAGIC_SIZE,
                             scratch + BK3633_NVDS_ABI_MAGIC_SIZE,
                             target - BK3633_NVDS_ABI_MAGIC_SIZE) != NVDS_OK))) {
        status = NVDS_FAIL;
    }
    if (scratch != NULL) {
        h2_pal_mem_free(mem, scratch);
    }
    return status;
}

static uint8_t bk3633_nvds_compact_and_put(uint8_t tag,
                                            uint8_t length,
                                            const uint8_t *data) {
    return bk3633_nvds_compact(tag, length, data, true);
}

h2_pal_result_t h2_bk3633_sdk_runtime_nvds_maintain(void) {
    uint32_t offset = BK3633_NVDS_ABI_MAGIC_SIZE;
    bk3633_nvds_record_t record;
    bool reclaimable = false;
    uint8_t status;

    for (;;) {
        status = bk3633_nvds_walk(offset, &record);
        if (status == NVDS_TAG_NOT_DEFINED) {
            if (BK3633_NVDS_ABI_SIZE - offset >=
                BK3633_NVDS_ABI_MAINTENANCE_RESERVE) {
                return H2_PAL_OK;
            }
            status = reclaimable
                         ? bk3633_nvds_compact(0u, 0u, NULL, false)
                         : NVDS_NO_SPACE_AVAILABLE;
            break;
        }
        if (status == NVDS_CORRUPT) {
            status = bk3633_nvds_compact(0u, 0u, NULL, false);
            break;
        }
        if (status != NVDS_OK) {
            break;
        }
        reclaimable = reclaimable || !bk3633_nvds_record_valid(&record);
        offset = record.next;
    }
    return status == NVDS_OK
               ? H2_PAL_OK
               : (status == NVDS_NO_SPACE_AVAILABLE
                      ? H2_PAL_ERR_NO_SPACE
                      : H2_PAL_ERR_IO);
}

uint8_t nvds_put(uint8_t tag, nvds_tag_len_t length, uint8_t *data) {
    uint32_t offset = BK3633_NVDS_ABI_MAGIC_SIZE;
    bk3633_nvds_record_t record;
    bk3633_nvds_record_t prior = {0};
    bool have_prior = false;
    bool duplicate = false;
    uint8_t status;

    if (data == NULL && length != 0u) {
        return NVDS_FAIL;
    }
    for (;;) {
        status = bk3633_nvds_walk(offset, &record);
        if (status == NVDS_TAG_NOT_DEFINED) {
            break;
        }
        if (status == NVDS_CORRUPT) {
            return bk3633_nvds_compact_and_put(tag, length, data);
        }
        if (status != NVDS_OK) {
            return status;
        }
        if (record.header.tag == tag && bk3633_nvds_record_valid(&record)) {
            if (bk3633_nvds_record_locked(&record)) {
                return NVDS_PARAM_LOCKED;
            }
            if (have_prior) {
                duplicate = true;
            } else {
                prior = record;
                have_prior = true;
            }
        }
        offset = record.next;
    }
    if (duplicate) {
        return bk3633_nvds_compact_and_put(tag, length, data);
    }
    if (have_prior) {
        bool equal;
        status = bk3633_nvds_value_equal(&prior, data, length, &equal);
        if (status != NVDS_OK || equal) {
            return status;
        }
    }
    status = bk3633_nvds_append(offset, tag, length, data);
    if (status == NVDS_NO_SPACE_AVAILABLE) {
        return bk3633_nvds_compact_and_put(tag, length, data);
    }
    if (status == NVDS_OK && have_prior) {
        status = bk3633_nvds_mark(
            prior.offset,
            (uint8_t)(prior.header.status &
                      ~BK3633_NVDS_ABI_STATUS_ERASED_MASK));
    }
    return status;
}
