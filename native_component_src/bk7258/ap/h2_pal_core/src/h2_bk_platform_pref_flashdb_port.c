#include "driver/flash.h"
#include "driver/flash_partition.h"
#include "flashdb.h"
#include "os/os.h"

#include <stdint.h>
#include <string.h>

#define H2_BK_FLASHDB_ERASE_SIZE (4u * 1024u)

static beken_mutex_t s_flashdb_flash_mutex;
/* SDK GC ignores some intermediate copy failures. Once hardware reports IO,
 * reject the rest of this operation's writes/erases, particularly deletion of
 * the source sector. The provider reinitializes both real DBs before retry. */
static int s_flashdb_io_failed;
int h2_bk_pref_flash_faulted(void) { return s_flashdb_io_failed; }
void h2_bk_pref_flash_clear_fault(void) { s_flashdb_io_failed = 0; }

static int bk_pref_flash_init(void) {
    const bk_logic_partition_t *partition =
        bk_flash_partition_get_info(BK_PARTITION_FLASHDB);

    if (partition == NULL) {
        return -1;
    }
    if (partition->partition_start_addr !=
            CONFIG_FLASHDB_KVDB_START_ADDR ||
        CONFIG_FLASHDB_KVDB_SIZE != 0x6000u ||
        H2_BK_PREF_LARGE_OFFSET + H2_BK_PREF_LARGE_SIZE !=
            partition->partition_start_addr + partition->partition_length) {
        return -1;
    }
    if (s_flashdb_flash_mutex == NULL &&
        rtos_init_mutex(&s_flashdb_flash_mutex) != kNoErr) {
        return -1;
    }
    g_flashdb0.len =
        partition->partition_start_addr + partition->partition_length;
    return 0;
}

/* Invalid/unknown nonempty tail sectors must never be auto-formatted. The
 * only implicit initialization allowed is an entirely erased new DB. */
int h2_bk_pref_large_is_blank(void) {
    uint8_t bytes[128];
    int blank = 1;
    if (rtos_lock_mutex(&s_flashdb_flash_mutex) != kNoErr) return -1;
    for (uint32_t offset = 0; offset < H2_BK_PREF_LARGE_SIZE; offset += sizeof(bytes)) {
        if (bk_flash_read_bytes(H2_BK_PREF_LARGE_OFFSET + offset, bytes, sizeof(bytes)) != BK_OK) {
            s_flashdb_io_failed = 1;
            blank = -1;
            break;
        }
        for (size_t i = 0; i < sizeof(bytes); ++i)
            if (bytes[i] != 0xffu) { blank = 0; break; }
        if (!blank) break;
    }
    (void)rtos_unlock_mutex(&s_flashdb_flash_mutex);
    return blank;
}

/* Owned sectors with invalid headers can be initialized only when their
 * entire payload is erased. This recovers a failed header program after GC
 * without erasing unknown contents or any surviving live record. */
int h2_bk_pref_large_empty_headers(void) {
    uint8_t bytes[128];
    int safe = 1;
    if (rtos_lock_mutex(&s_flashdb_flash_mutex) != kNoErr) return -1;
    for (uint32_t sector = 0; sector < H2_BK_PREF_LARGE_SIZE && safe; sector += H2_BK_FLASHDB_ERASE_SIZE) {
        uint32_t magic = 0;
        if (bk_flash_read_bytes(H2_BK_PREF_LARGE_OFFSET + sector + 8u, (uint8_t *)&magic, sizeof(magic)) != BK_OK) { safe = -1; break; }
        if (magic == 0x30424446u) continue; /* FlashDB 1.1.2 sector header */
        for (uint32_t offset = 20u; offset < H2_BK_FLASHDB_ERASE_SIZE;) {
            uint32_t size = H2_BK_FLASHDB_ERASE_SIZE - offset;
            if (size > sizeof(bytes)) size = sizeof(bytes);
            if (bk_flash_read_bytes(H2_BK_PREF_LARGE_OFFSET + sector + offset, bytes, size) != BK_OK) { safe = -1; break; }
            for (uint32_t i = 0; i < size; ++i) if (bytes[i] != 0xffu) { safe = 0; break; }
            if (!safe) break;
            offset += size;
        }
    }
    if (safe < 0) s_flashdb_io_failed = 1;
    (void)rtos_unlock_mutex(&s_flashdb_flash_mutex);
    return safe;
}

static int bk_pref_flash_read(long offset, uint8_t *buffer, size_t size) {
    bk_err_t rc;

    if (offset < 0 || buffer == NULL || size > UINT32_MAX) {
        return -1;
    }
    if (rtos_lock_mutex(&s_flashdb_flash_mutex) != kNoErr) {
        return -1;
    }
    rc = bk_flash_read_bytes((uint32_t)offset, buffer, (uint32_t)size);
    if (rc != BK_OK) {
        s_flashdb_io_failed = 1;
        /* SDK ignores some read return codes; deterministic erased bytes
         * keep its parser from following an uninitialized length/address. */
        memset(buffer, 0xff, size);
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
    rc = s_flashdb_io_failed ? -1 : bk_flash_write_bytes((uint32_t)offset, buffer, (uint32_t)size);
    if (rc != BK_OK) s_flashdb_io_failed = 1;
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

    if (s_flashdb_io_failed) {
        (void)rtos_unlock_mutex(&s_flashdb_flash_mutex);
        return -1;
    }
    protect_type = bk_flash_get_protect_type();
    if (protect_type != FLASH_PROTECT_NONE) {
        bk_flash_set_protect_type(FLASH_PROTECT_NONE);
    }
    address = (uint32_t)offset;
    while (erased < size) {
        rc = bk_flash_erase_sector(address);
        if (rc != BK_OK) {
            s_flashdb_io_failed = 1;
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
