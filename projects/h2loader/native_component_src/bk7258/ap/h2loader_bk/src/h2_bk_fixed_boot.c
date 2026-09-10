#ifdef H2_BK_FIXED_BOOT_HOST_TEST
#include "h2_bk_fixed_boot_test_sdk.h"
#else
#include "h2_bk_fixed_boot.h"
#include "h2_bk_h2loader.h"
#include "driver/flash.h"
#include "driver/flash_partition.h"
#include "modules/ota.h"
#endif
#include "layout.h"
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* SDK execution flags final=A, temporary=A, confirm=A. Native boot never
 * leaves Loader in the fixed layout. */
static const uint32_t s_native_loader_flags[3] = {0u, 0u, 3u};

int h2_bk_fixed_layout_active(void) {
    const bk_logic_partition_t *cp = bk_flash_partition_get_info(BK_PARTITION_APPLICATION);
    const bk_logic_partition_t *ap = bk_flash_partition_get_info(BK_PARTITION_APPLICATION1);
    if (!cp || !ap || cp->partition_length != H2_FIXED_CP_SIZE) return 0;
    return (cp->partition_start_addr == H2_FIXED_LOADER_OFFSET &&
            ap->partition_length == H2_FIXED_LOADER_AP_SIZE) ||
           (cp->partition_start_addr == H2_FIXED_APP_OFFSET &&
            ap->partition_length == H2_FIXED_APP_SIZE - H2_FIXED_CP_SIZE);
}

static void log_role_once(uint8_t slot, uint32_t cp_phys) {
    static int logged;
    uint32_t pc = 0u;
    if (logged) return;
    logged = 1;
#if defined(__arm__)
    __asm__ volatile("mov %0, pc" : "=r"(pc));
#endif
    printf("H2_FIXED_XIP role=%s pc=%08lx cp_phys=%08lx native_slot=%u\r\n",
        slot ? "app" : "loader", (unsigned long)pc, (unsigned long)cp_phys,
        (unsigned)bk_ota_get_current_partition());
}

uint8_t h2_bk_fixed_current_slot(void) {
    if (!h2_bk_fixed_layout_active()) return bk_ota_get_current_partition();
    const bk_logic_partition_t *cp = bk_flash_partition_get_info(BK_PARTITION_APPLICATION);
    uint8_t slot = cp->partition_start_addr == H2_FIXED_APP_OFFSET ? 1u : 0u;
    log_role_once(slot, cp->partition_start_addr);
    return slot;
}

static int read_request(h2_fixed_boot_request_t *r) {
    return bk_flash_read_bytes(H2_FIXED_CONTROL_OFFSET, (uint8_t *)r, sizeof(*r)) == BK_OK;
}

int h2_bk_fixed_next_app(void) {
    h2_fixed_boot_request_t r;
    return read_request(&r) && h2_fixed_request_boots_app(&r);
}

int h2_bk_fixed_app_failed(void) {
    h2_fixed_boot_request_t r;
    return read_request(&r) && h2_fixed_request_failed(&r);
}

static int program(uint32_t address, const void *data, uint32_t size, int erase) {
    flash_protect_type_t protect = bk_flash_get_protect_type();
    if (bk_flash_set_protect_type(FLASH_PROTECT_NONE) != BK_OK) return BK_FAIL;
    int rc = erase ? bk_flash_erase_sector(address) : BK_OK;
    if (rc == BK_OK && size != 0u) rc = bk_flash_write_bytes(address, (const uint8_t *)data, size);
    (void)bk_flash_set_protect_type(protect);
    return rc;
}

static int keep_native_loader_boot(void) {
    const bk_logic_partition_t *control = bk_flash_partition_get_info(BK_PARTITION_OTA_FINA_EXECUTIVE);
    uint32_t current[3];
    if (!control) return H2_PAL_ERR_INVALID_STATE;
    if (bk_flash_read_bytes(control->partition_start_addr, (uint8_t *)current,
                            sizeof(current)) != BK_OK) return H2_PAL_ERR_IO;
    /* Normally already A/A/A: skip the erase so every App boot does not wear
     * the SDK sector or open a window where it is blank. */
    if (memcmp(current, s_native_loader_flags, sizeof(current)) == 0) return H2_PAL_OK;
    return program(control->partition_start_addr, s_native_loader_flags,
                   sizeof(s_native_loader_flags), 1) == BK_OK ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static int request_is_blank(const h2_fixed_boot_request_t *r) {
    static const uint8_t erased[sizeof(*r)] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    };
    return memcmp(r, erased, sizeof(*r)) == 0;
}

int h2_bk_fixed_select(uint32_t partition_id) {
    h2_fixed_boot_request_t current;
    if (partition_id != H2_BK_H2LOADER_PRIMARY_PARTITION_ID &&
        partition_id != H2_BK_H2LOADER_APP_PARTITION_ID) return H2_PAL_ERR_INVALID_ARG;
    int rc = keep_native_loader_boot();
    if (rc != H2_PAL_OK) return rc;
    if (!read_request(&current)) return H2_PAL_ERR_IO;
    if (partition_id == H2_BK_H2LOADER_PRIMARY_PARTITION_ID) {
        /* Loader selection clears any request and the failed-attempt record. */
        if (request_is_blank(&current)) return H2_PAL_OK;
        rc = program(H2_FIXED_CONTROL_OFFSET, NULL, 0u, 1);
        return rc == BK_OK && read_request(&current) && request_is_blank(&current)
            ? H2_PAL_OK : H2_PAL_ERR_IO;
    }
    h2_fixed_boot_request_t r = {
        .magic = H2_FIXED_REQUEST_MAGIC, .app_offset = H2_FIXED_APP_OFFSET,
        .app_size = H2_FIXED_APP_SIZE,
        .check = ~(H2_FIXED_REQUEST_MAGIC ^ H2_FIXED_APP_OFFSET ^ H2_FIXED_APP_SIZE),
        .confirmed = H2_FIXED_ERASED,
    };
    /* Keep a pending trial. A confirmed record is rewritten as a new trial:
     * if Loader is running despite it, CP rejected App, and the trial lets
     * that rejection surface as a failed attempt instead of a reboot loop. */
    if (h2_fixed_request_valid(&current)) return H2_PAL_OK;
    /* Publish magic last and leave confirmed erased. Interrupted writes
     * cannot form a valid request. */
    rc = program(H2_FIXED_CONTROL_OFFSET, NULL, 0u, 1);
    if (rc == BK_OK) rc = program(H2_FIXED_CONTROL_OFFSET + 4u, &r.app_offset,
                                  (uint32_t)(offsetof(h2_fixed_boot_request_t, confirmed) - 4u), 0);
    if (rc == BK_OK) rc = program(H2_FIXED_CONTROL_OFFSET, &r.magic, sizeof(r.magic), 0);
    if (rc != BK_OK || !read_request(&current) || memcmp(&r, &current, sizeof(r)) != 0) {
        return H2_PAL_ERR_IO;
    }
    return H2_PAL_OK;
}

int h2_bk_fixed_confirm_app(void) {
    h2_fixed_boot_request_t r;
    static const uint32_t confirmed = 0u;
    if (!read_request(&r)) return H2_PAL_ERR_IO;
    /* CP only enters App after consuming a request, so any other record
     * means there is no attempt left to confirm. */
    if (!h2_fixed_request_failed(&r)) return H2_PAL_OK;
    if (program(H2_FIXED_CONTROL_OFFSET + (uint32_t)offsetof(h2_fixed_boot_request_t, confirmed),
                &confirmed, sizeof(confirmed), 0) != BK_OK ||
        !read_request(&r) || r.confirmed != 0u) {
        return H2_PAL_ERR_IO;
    }
    return H2_PAL_OK;
}

int h2_bk_fixed_invalidate_app(void) {
    /* Called before App Flash is erased: CP must never enter a partial image. */
    return h2_bk_fixed_select(H2_BK_H2LOADER_PRIMARY_PARTITION_ID);
}
