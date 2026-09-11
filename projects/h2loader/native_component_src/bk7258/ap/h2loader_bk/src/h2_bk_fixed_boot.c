#ifdef H2_BK_FIXED_BOOT_HOST_TEST
#include "h2_bk_fixed_boot_test_sdk.h"
#else
#include "h2_bk_fixed_boot.h"
#include "h2_bk_h2loader.h"
#include "driver/flash.h"
#include "driver/flash_partition.h"
#include "modules/ota.h"
#include "layout_check.h"
#endif
#include "layout.h"
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* SDK execution flags final=A, temporary=A, confirm=A: native boot runs the
 * Loader window. */
static const uint32_t s_native_loader_flags[3] = {0u, 0u, 3u};
/* A Loader update borrows the App window as the native B slot. The ROM
 * bootloader then remaps the Loader window's addresses onto the App window,
 * so a Loader image (linked for the Loader window) runs from there while it
 * rewrites the Loader window. Byte layout as the SDK writes it: final, temp
 * and confirm at offsets 0/4/8; confirm 1 = pending after an update, 4 = B
 * confirmed. */
static const uint8_t s_native_relay_pending[12] = {
    1u, 0xffu, 0xffu, 0xffu, 1u, 0xffu, 0xffu, 0xffu, 1u, 0xffu, 0xffu, 0xffu,
};
static const uint8_t s_native_relay_confirmed[12] = {
    1u, 0xffu, 0xffu, 0xffu, 1u, 0xffu, 0xffu, 0xffu, 4u, 0xffu, 0xffu, 0xffu,
};

static h2_fixed_window_t partition_window(bk_partition_t id) {
    const bk_logic_partition_t *p = bk_flash_partition_get_info(id);
    return p ? (h2_fixed_window_t){p->partition_start_addr, p->partition_length}
             : (h2_fixed_window_t){0u, 0u};
}

const h2_fixed_layout_t *h2_bk_fixed_layout(void) {
    /* The board's partition table decides the windows; a table without the
     * h2_boot_request partition is not a fixed layout. */
#ifdef BK_PARTITION_H2_BOOT_REQUEST
    static h2_fixed_layout_t layout;
    return h2_fixed_layout_from_partitions(
               partition_window(BK_PARTITION_APPLICATION),
               partition_window(BK_PARTITION_APPLICATION1),
               partition_window(BK_PARTITION_S_APP),
               partition_window(BK_PARTITION_H2_BOOT_REQUEST), &layout)
        ? &layout : NULL;
#else
    return NULL;
#endif
}

int h2_bk_fixed_layout_active(void) {
    return h2_bk_fixed_layout() != NULL;
}

static void log_role_once(uint8_t slot, int app_table, uint32_t cp_phys) {
    static int logged;
    uint32_t pc = 0u;
    if (logged) return;
    logged = 1;
#if defined(__arm__)
    __asm__ volatile("mov %0, pc" : "=r"(pc));
#endif
    printf("H2_FIXED_XIP role=%s pc=%08lx cp_phys=%08lx native_slot=%u\r\n",
        app_table ? "app" : slot ? "loader-relay" : "loader",
        (unsigned long)pc, (unsigned long)cp_phys,
        (unsigned)bk_ota_get_current_partition());
}

/* The native slot reports the ROM bootloader's A/B remap: 1 while the Loader
 * window's addresses execute from the App window. */
static int native_relay_active(void) {
    return bk_ota_get_current_partition() == 1u;
}

uint8_t h2_bk_fixed_current_slot(void) {
    const h2_fixed_layout_t *layout = h2_bk_fixed_layout();
    if (layout == NULL) return bk_ota_get_current_partition();
    /* App images are linked for the App window; a Loader image runs there
     * only through the native remap during a Loader update. */
    uint8_t slot = layout->app_table || native_relay_active() ? 1u : 0u;
    log_role_once(slot, layout->app_table,
                  layout->app_table ? layout->app.offset : layout->loader.offset);
    return slot;
}

static int read_request(h2_fixed_boot_request_t *r) {
    const h2_fixed_layout_t *layout = h2_bk_fixed_layout();
    return layout != NULL &&
        bk_flash_read_bytes(layout->control_offset, (uint8_t *)r, sizeof(*r)) == BK_OK;
}

static int read_native_flags(uint8_t out[12]) {
    const bk_logic_partition_t *control = bk_flash_partition_get_info(BK_PARTITION_OTA_FINA_EXECUTIVE);
    return control != NULL &&
        bk_flash_read_bytes(control->partition_start_addr, out, 12u) == BK_OK;
}

/* final=B: the next reset runs the candidate Loader in the App window. */
static int native_selects_relay(void) {
    uint8_t flags[12];
    return read_native_flags(flags) && flags[0] == 1u;
}

int h2_bk_fixed_relay_failed(void) {
    /* The ROM bootloader tries a pending B once and, when the candidate never
     * confirms, returns to A leaving final=A, temp=B, confirm=pending. Seen
     * from the Loader window this proves the candidate Loader failed. */
    uint8_t flags[12];
    return h2_bk_fixed_layout() != NULL && !native_relay_active() &&
        read_native_flags(flags) && flags[0] == 0u && flags[4] == 1u && flags[8] == 1u;
}

int h2_bk_fixed_next_app(void) {
    h2_fixed_boot_request_t r;
    return native_selects_relay() ||
        (read_request(&r) && h2_fixed_request_boots_app(&r, h2_bk_fixed_layout()));
}

int h2_bk_fixed_app_failed(void) {
    h2_fixed_boot_request_t r;
    return read_request(&r) && h2_fixed_request_failed(&r, h2_bk_fixed_layout());
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

static int write_native_flags(const uint8_t flags[12]) {
    const bk_logic_partition_t *control = bk_flash_partition_get_info(BK_PARTITION_OTA_FINA_EXECUTIVE);
    uint8_t current[12];
    if (!control || !read_native_flags(current)) return H2_PAL_ERR_IO;
    if (memcmp(current, flags, sizeof(current)) == 0) return H2_PAL_OK;
    if (program(control->partition_start_addr, flags, 12u, 1) != BK_OK ||
        !read_native_flags(current) || memcmp(current, flags, sizeof(current)) != 0) {
        return H2_PAL_ERR_IO;
    }
    return H2_PAL_OK;
}

int h2_bk_fixed_app_window_holds_loader(void) {
    /* The first CRC block of the App window starts with the CP vector table
     * (32 data bytes + 2 CRC bytes). Its reset handler says which window the
     * image was linked for. */
    const h2_fixed_layout_t *layout = h2_bk_fixed_layout();
    uint8_t block[34];
    uint32_t pc;
    if (layout == NULL ||
        bk_flash_read_bytes(layout->app.offset, block, sizeof(block)) != BK_OK) return 0;
    memcpy(&pc, &block[4], sizeof(pc));
    return h2_fixed_xip_in_window(pc, &layout->loader);
}

int h2_bk_fixed_confirm_loader(void) {
    /* A Loader running through the remap confirms B, so a reset while it
     * rewrites the Loader window returns to it instead of the half-written
     * Loader window. */
    return native_relay_active() ? write_native_flags(s_native_relay_confirmed) : H2_PAL_OK;
}

static int request_is_blank(const h2_fixed_boot_request_t *r) {
    static const uint8_t erased[sizeof(*r)] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    };
    return memcmp(r, erased, sizeof(*r)) == 0;
}

int h2_bk_fixed_select(uint32_t partition_id) {
    const h2_fixed_layout_t *layout = h2_bk_fixed_layout();
    h2_fixed_boot_request_t current;
    if (layout == NULL) return H2_PAL_ERR_INVALID_STATE;
    if (partition_id != H2_BK_H2LOADER_PRIMARY_PARTITION_ID &&
        partition_id != H2_BK_H2LOADER_APP_PARTITION_ID) return H2_PAL_ERR_INVALID_ARG;
    if (partition_id == H2_BK_H2LOADER_APP_PARTITION_ID && h2_bk_fixed_app_window_holds_loader()) {
        /* A Loader in the App window runs through the native B remap, not
         * through the CP handoff; keep an existing B selection as it is. */
        if (native_selects_relay()) return H2_PAL_OK;
        int rc = h2_bk_fixed_select(H2_BK_H2LOADER_PRIMARY_PARTITION_ID);
        return rc == H2_PAL_OK ? write_native_flags(s_native_relay_pending) : rc;
    }
    int rc = keep_native_loader_boot();
    if (rc != H2_PAL_OK) return rc;
    if (!read_request(&current)) return H2_PAL_ERR_IO;
    if (partition_id == H2_BK_H2LOADER_PRIMARY_PARTITION_ID) {
        /* Loader selection clears any request and the failed-attempt record. */
        if (request_is_blank(&current)) return H2_PAL_OK;
        rc = program(layout->control_offset, NULL, 0u, 1);
        return rc == BK_OK && read_request(&current) && request_is_blank(&current)
            ? H2_PAL_OK : H2_PAL_ERR_IO;
    }
    h2_fixed_boot_request_t r = {
        .magic = H2_FIXED_REQUEST_MAGIC, .app_offset = layout->app.offset,
        .app_size = layout->app.size,
        .check = ~(H2_FIXED_REQUEST_MAGIC ^ layout->app.offset ^ layout->app.size),
        .confirmed = H2_FIXED_ERASED,
    };
    /* Keep a pending trial. A confirmed record is rewritten as a new trial:
     * if Loader is running despite it, CP rejected App, and the trial lets
     * that rejection surface as a failed attempt instead of a reboot loop. */
    if (h2_fixed_request_valid(&current, layout)) return H2_PAL_OK;
    /* Publish magic last and leave confirmed erased. Interrupted writes
     * cannot form a valid request. */
    rc = program(layout->control_offset, NULL, 0u, 1);
    if (rc == BK_OK) rc = program(layout->control_offset + 4u, &r.app_offset,
                                  (uint32_t)(offsetof(h2_fixed_boot_request_t, confirmed) - 4u), 0);
    if (rc == BK_OK) rc = program(layout->control_offset, &r.magic, sizeof(r.magic), 0);
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
    if (!h2_fixed_request_failed(&r, h2_bk_fixed_layout())) return H2_PAL_OK;
    if (program(h2_bk_fixed_layout()->control_offset +
                (uint32_t)offsetof(h2_fixed_boot_request_t, confirmed),
                &confirmed, sizeof(confirmed), 0) != BK_OK ||
        !read_request(&r) || r.confirmed != 0u) {
        return H2_PAL_ERR_IO;
    }
    return H2_PAL_OK;
}

/* The ROM bootloader validates slot B from the RBL head at the end of the
 * slot, while a Loader image is sized for the smaller Loader window. Copy the
 * image's last RBL head area to the end of the App window so B validates
 * against the Loader image written at the window start. The head is held in
 * RAM and each touched sector is read, patched and rewritten, so image bytes
 * sharing a sector with the destination survive the erase. */
#define H2_BK_RELAY_HEAD_MAX 0x1100u
static uint8_t s_relay_head[H2_BK_RELAY_HEAD_MAX];
static uint8_t s_relay_sector[H2_FIXED_SECTOR_SIZE];

int h2_bk_fixed_publish_relay_head(uint32_t window_offset, uint32_t window_size,
                                   uint32_t image_size, uint32_t head_size) {
    uint32_t offset = 0u;
    const int plan = h2_fixed_relay_head_offset(window_size, image_size, head_size, &offset);
    if (plan < 0 || head_size > sizeof(s_relay_head)) return H2_PAL_ERR_INVALID_STATE;
    if (plan == 0) return H2_PAL_OK;
    if (bk_flash_read_bytes(window_offset + image_size - head_size, s_relay_head,
                            head_size) != BK_OK) {
        return H2_PAL_ERR_IO;
    }
    const uint32_t destination = window_offset + offset;
    const uint32_t window_end = window_offset + window_size;
    for (uint32_t sector = destination & ~(H2_FIXED_SECTOR_SIZE - 1u); sector < window_end;
         sector += H2_FIXED_SECTOR_SIZE) {
        const uint32_t from = sector > destination ? sector : destination;
        const uint32_t to = sector + H2_FIXED_SECTOR_SIZE < window_end
            ? sector + H2_FIXED_SECTOR_SIZE : window_end;
        if (bk_flash_read_bytes(sector, s_relay_sector, sizeof(s_relay_sector)) != BK_OK) {
            return H2_PAL_ERR_IO;
        }
        memcpy(&s_relay_sector[from - sector], &s_relay_head[from - destination], to - from);
        if (program(sector, s_relay_sector, sizeof(s_relay_sector), 1) != BK_OK) {
            return H2_PAL_ERR_IO;
        }
    }
    printf("H2_BK_OTA_WRITER stage=relay_head offset=%08lx\r\n", (unsigned long)destination);
    return H2_PAL_OK;
}

int h2_bk_fixed_invalidate_app(void) {
    /* Called before App Flash is erased: CP must never enter a partial image. */
    return h2_bk_fixed_select(H2_BK_H2LOADER_PRIMARY_PARTITION_ID);
}
