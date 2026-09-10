#ifndef H2_BK_FIXED_FLASH_LAYOUT_H
#define H2_BK_FIXED_FLASH_LAYOUT_H
#include <stdint.h>
/* The board's partition tables own every offset and size. Each image carries
 * its own table: the Loader table lists the Loader window as
 * primary_cp_app + primary_ap_app and the App window as s_app; the App table
 * lists the App window as primary_cp_app + primary_ap_app and the Loader
 * window as s_app. Both list the same h2_boot_request sector. Executable
 * images contain 32 data bytes + 2 CRC bytes per block.
 */
#define H2_FIXED_REQUEST_MAGIC 0x58495031u
#define H2_FIXED_ERASED 0xffffffffu
#define H2_FIXED_SECTOR_SIZE 4096u
#define H2_FIXED_WINDOW_ALIGN (68u * 1024u)
#define H2_FIXED_XIP_ADDRESS(offset) (0x02000000u + ((offset) / 34u) * 32u)

typedef struct h2_fixed_window {
    uint32_t offset;
    uint32_t size;
} h2_fixed_window_t;

typedef struct h2_fixed_layout {
    h2_fixed_window_t loader;
    h2_fixed_window_t app;
    uint32_t control_offset;
    /* 1 when these partitions are the App table (this image is the App). */
    int app_table;
} h2_fixed_layout_t;

static inline int h2_fixed_window_end(const h2_fixed_window_t *w, uint32_t *out) {
    if (w->size == 0u || w->offset > UINT32_MAX - w->size) return 0;
    *out = w->offset + w->size;
    return 1;
}

/* Derive both windows from one image's partition table. own_cp/own_ap are the
 * executing image's CP and AP partitions, other is s_app. Returns 0 when the
 * table is not a fixed Loader/App layout. */
static inline int h2_fixed_layout_from_partitions(
    h2_fixed_window_t own_cp, h2_fixed_window_t own_ap, h2_fixed_window_t other,
    h2_fixed_window_t control, h2_fixed_layout_t *out) {
    uint32_t cp_end, ap_end, other_end, control_end;
    const h2_fixed_window_t own = {own_cp.offset, own_cp.size + own_ap.size};
    if (!h2_fixed_window_end(&own_cp, &cp_end) || !h2_fixed_window_end(&own_ap, &ap_end) ||
        !h2_fixed_window_end(&other, &other_end) || !h2_fixed_window_end(&control, &control_end) ||
        cp_end != own_ap.offset || control.size < H2_FIXED_SECTOR_SIZE ||
        control.offset % H2_FIXED_SECTOR_SIZE != 0u ||
        /* Windows start on a 4 KiB erase sector and a 34-byte CRC block, so
         * board sizes are multiples of 68 KiB. */
        own_cp.offset % H2_FIXED_WINDOW_ALIGN != 0u || other.offset % H2_FIXED_WINDOW_ALIGN != 0u ||
        own.size % H2_FIXED_WINDOW_ALIGN != 0u || other.size % H2_FIXED_WINDOW_ALIGN != 0u) {
        return 0;
    }
    /* The two executable windows and the control sector must not overlap. */
    if (!(ap_end <= other.offset || other_end <= own.offset) ||
        !(control_end <= own.offset || ap_end <= control.offset) ||
        !(control_end <= other.offset || other_end <= control.offset)) {
        return 0;
    }
    out->app_table = other.offset < own.offset;
    out->loader = out->app_table ? other : own;
    out->app = out->app_table ? own : other;
    out->control_offset = control.offset;
    return 1;
}

/* One record per erase. Loader writes a request after erasing the sector;
 * CP consumes it by clearing magic before the trial boot; App confirms by
 * clearing confirmed. Both later transitions only clear bits, so they need no
 * erase. A confirmed record makes CP enter App on every reset, like a
 * confirmed native B; a consumed, unconfirmed one returns to Loader. */
typedef struct h2_fixed_boot_request {
    uint32_t magic;
    uint32_t app_offset;
    uint32_t app_size;
    uint32_t check;
    uint32_t confirmed;
} h2_fixed_boot_request_t;
static inline int h2_fixed_request_body_valid(const h2_fixed_boot_request_t *r,
                                              const h2_fixed_layout_t *layout) {
    return r->app_offset == layout->app.offset && r->app_size == layout->app.size &&
        r->check == ~(H2_FIXED_REQUEST_MAGIC ^ r->app_offset ^ r->app_size);
}
static inline int h2_fixed_request_valid(const h2_fixed_boot_request_t *r,
                                         const h2_fixed_layout_t *layout) {
    return r->magic == H2_FIXED_REQUEST_MAGIC && r->confirmed == H2_FIXED_ERASED &&
        h2_fixed_request_body_valid(r, layout);
}
static inline int h2_fixed_request_consumed(const h2_fixed_boot_request_t *r,
                                            const h2_fixed_layout_t *layout) {
    return r->magic == 0u && h2_fixed_request_body_valid(r, layout);
}
static inline int h2_fixed_request_confirmed(const h2_fixed_boot_request_t *r,
                                             const h2_fixed_layout_t *layout) {
    return h2_fixed_request_consumed(r, layout) && r->confirmed == 0u;
}
static inline int h2_fixed_request_failed(const h2_fixed_boot_request_t *r,
                                          const h2_fixed_layout_t *layout) {
    return h2_fixed_request_consumed(r, layout) && r->confirmed == H2_FIXED_ERASED;
}
/* Whether CP enters App on the next reset. */
static inline int h2_fixed_request_boots_app(const h2_fixed_boot_request_t *r,
                                             const h2_fixed_layout_t *layout) {
    return h2_fixed_request_valid(r, layout) || h2_fixed_request_confirmed(r, layout);
}
/* Where a Loader image staged at the start of the App window must also carry
 * its RBL head area (the image's last head_size bytes) so the ROM bootloader
 * finds it at the end of slot B. Returns 1 and the window offset to copy to,
 * 0 when the head already sits at the window end, -1 when the copy would
 * overlap the image. */
static inline int h2_fixed_relay_head_offset(uint32_t window_size, uint32_t image_size,
                                             uint32_t head_size, uint32_t *out_offset) {
    if (image_size < head_size || image_size > window_size) return -1;
    if (image_size == window_size) return 0;
    if (window_size - image_size < head_size) return -1;
    *out_offset = window_size - head_size;
    return 1;
}
/* Whether a physical PC lies in a window's XIP range. */
static inline int h2_fixed_xip_in_window(uint32_t pc, const h2_fixed_window_t *w) {
    return pc >= H2_FIXED_XIP_ADDRESS(w->offset) &&
           pc < H2_FIXED_XIP_ADDRESS(w->offset + w->size);
}
#endif
