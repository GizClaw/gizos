#include "driver/flash.h"
#include "driver/flash_partition.h"
#include "flashdb.h"
#include "os/os.h"
#include "os/mem.h"

#include <stdint.h>
#include <string.h>

#define H2_BK_FLASHDB_ERASE_SIZE (4u * 1024u)
#define H2_BK_FLASHDB_READ_CACHE_SIZE 512u

static beken_mutex_t s_flashdb_flash_mutex;
/* FlashDB scans headers and CRC payloads in small adjacent reads. Cache only
 * bytes actually read from this partition; never populate it from writes. */
/* FAL has process lifetime and no deinit API. Keep the optional cache in
 * PSRAM so Loader/App retain the existing internal-heap budget. */
static uint8_t *s_flashdb_read_cache;
static uint32_t s_flashdb_read_cache_offset;
static size_t s_flashdb_read_cache_size;

static int bk_pref_flash_init(void) {
    const bk_logic_partition_t *partition =
        bk_flash_partition_get_info(BK_PARTITION_FLASHDB);

    if (partition == NULL) {
        return -1;
    }
    if (partition->partition_start_addr !=
            CONFIG_FLASHDB_KVDB_START_ADDR ||
        CONFIG_FLASHDB_KVDB_SIZE > partition->partition_length) {
        return -1;
    }
    if (s_flashdb_flash_mutex == NULL &&
        rtos_init_mutex(&s_flashdb_flash_mutex) != kNoErr) {
        return -1;
    }
    if (rtos_lock_mutex(&s_flashdb_flash_mutex) != kNoErr) {
        return -1;
    }
    g_flashdb0.len =
        partition->partition_start_addr + partition->partition_length;
    s_flashdb_read_cache_size = 0u;
    if (s_flashdb_read_cache == NULL)
        s_flashdb_read_cache = psram_malloc(H2_BK_FLASHDB_READ_CACHE_SIZE);
    (void)rtos_unlock_mutex(&s_flashdb_flash_mutex);
    return 0;
}

static int bk_pref_flash_read(long offset, uint8_t *buffer, size_t size) {
    bk_err_t rc;

    if (offset < 0 || buffer == NULL || size > UINT32_MAX) {
        return -1;
    }
    if (rtos_lock_mutex(&s_flashdb_flash_mutex) != kNoErr) {
        return -1;
    }
    const uint32_t address = (uint32_t)offset;
    const uint64_t partition_end = (uint64_t)CONFIG_FLASHDB_KVDB_START_ADDR +
                                   CONFIG_FLASHDB_KVDB_SIZE;
    uint32_t cache_offset = address & ~(H2_BK_FLASHDB_READ_CACHE_SIZE - 1u);
    if (cache_offset < CONFIG_FLASHDB_KVDB_START_ADDR)
        cache_offset = CONFIG_FLASHDB_KVDB_START_ADDR;
    const size_t cache_size = cache_offset < partition_end
        ? (size_t)(partition_end - cache_offset < H2_BK_FLASHDB_READ_CACHE_SIZE
            ? partition_end - cache_offset : H2_BK_FLASHDB_READ_CACHE_SIZE)
        : 0u;
    if (s_flashdb_read_cache != NULL && size != 0u && address >= cache_offset &&
        (uint64_t)address < partition_end &&
        address - cache_offset < cache_size &&
        size <= cache_size - (address - cache_offset)) {
        if (s_flashdb_read_cache_size == 0u ||
            address < s_flashdb_read_cache_offset ||
            (uint64_t)address + size > (uint64_t)s_flashdb_read_cache_offset +
                                       s_flashdb_read_cache_size) {
            s_flashdb_read_cache_size = 0u;
            rc = bk_flash_read_bytes(cache_offset, s_flashdb_read_cache,
                                     (uint32_t)cache_size);
            if (rc == BK_OK) {
                s_flashdb_read_cache_offset = cache_offset;
                s_flashdb_read_cache_size = cache_size;
            }
        } else {
            rc = BK_OK;
        }
        if (rc == BK_OK)
            memcpy(buffer, s_flashdb_read_cache +
                           (address - s_flashdb_read_cache_offset), size);
    } else {
        rc = bk_flash_read_bytes(address, buffer, (uint32_t)size);
    }
    (void)rtos_unlock_mutex(&s_flashdb_flash_mutex);
    return rc == BK_OK ? (int)size : -1;
}

static int bk_pref_flash_write(long offset, const uint8_t *buffer, size_t size) {
    bk_err_t rc;

    if (offset < 0 || buffer == NULL || size > UINT32_MAX) {
        return -1;
    }
    if (rtos_lock_mutex(&s_flashdb_flash_mutex) != kNoErr) {
        return -1;
    }
    /* The SDK can fail after programming bytes; invalidate before the call. */
    s_flashdb_read_cache_size = 0u;
    rc = bk_flash_write_bytes((uint32_t)offset, buffer, (uint32_t)size);
    (void)rtos_unlock_mutex(&s_flashdb_flash_mutex);
    return rc == BK_OK ? (int)size : -1;
}

static int bk_pref_flash_erase(long offset, size_t size) {
    flash_protect_type_t protect_type;
    uint32_t address;
    size_t erased = 0u;
    bk_err_t rc = BK_OK;

    if (offset < 0 || size == 0u ||
        ((uint32_t)offset % H2_BK_FLASHDB_ERASE_SIZE) != 0u ||
        (size % H2_BK_FLASHDB_ERASE_SIZE) != 0u) {
        return -1;
    }
    if (rtos_lock_mutex(&s_flashdb_flash_mutex) != kNoErr) {
        return -1;
    }

    s_flashdb_read_cache_size = 0u;
    protect_type = bk_flash_get_protect_type();
    if (protect_type != FLASH_PROTECT_NONE) {
        bk_flash_set_protect_type(FLASH_PROTECT_NONE);
    }
    address = (uint32_t)offset;
    while (erased < size) {
        rc = bk_flash_erase_sector(address);
        if (rc != BK_OK) {
            break;
        }
        address += H2_BK_FLASHDB_ERASE_SIZE;
        erased += H2_BK_FLASHDB_ERASE_SIZE;
    }
    if (protect_type != FLASH_PROTECT_NONE) {
        bk_flash_set_protect_type(protect_type);
    }

    (void)rtos_unlock_mutex(&s_flashdb_flash_mutex);
    return rc == BK_OK ? (int)erased : -1;
}

struct fal_flash_dev g_flashdb0 = {
    .name = FLASHDB_DEV_NAME,
    .addr = 0u,
    .len = 0u,
    .blk_size = H2_BK_FLASHDB_ERASE_SIZE,
    .ops = {
        .init = bk_pref_flash_init,
        .read = bk_pref_flash_read,
        .write = bk_pref_flash_write,
        .erase = bk_pref_flash_erase,
    },
    .write_gran = 8u,
};
