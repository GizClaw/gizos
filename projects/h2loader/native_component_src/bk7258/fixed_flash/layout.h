#ifndef H2_BK_FIXED_FLASH_LAYOUT_H
#define H2_BK_FIXED_FLASH_LAYOUT_H
#include <stdint.h>
/* Physical offsets; executable images contain 32 data bytes + 2 CRC bytes.
 * The native bootloader always selects Loader. App has its own linked address.
 * Keep these constants synchronized with the Loader/App partition tables.
 */
#define H2_FIXED_LOADER_OFFSET (68u * 1024u)
#define H2_FIXED_CP_SIZE (1156u * 1024u)
#define H2_FIXED_LOADER_AP_SIZE (1224u * 1024u)
#define H2_FIXED_LOADER_SIZE (H2_FIXED_CP_SIZE + H2_FIXED_LOADER_AP_SIZE)
#define H2_FIXED_APP_OFFSET (H2_FIXED_LOADER_OFFSET + H2_FIXED_LOADER_SIZE)
#define H2_FIXED_APP_SIZE (5100u * 1024u)
#define H2_FIXED_CONTROL_OFFSET 0x0077f000u
#define H2_FIXED_REQUEST_MAGIC 0x58495031u
#define H2_FIXED_ERASED 0xffffffffu
#define H2_FIXED_XIP_ADDRESS(offset) (0x02000000u + ((offset) / 34u) * 32u)
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
static inline int h2_fixed_request_body_valid(const h2_fixed_boot_request_t *r) {
    return r->app_offset == H2_FIXED_APP_OFFSET && r->app_size == H2_FIXED_APP_SIZE &&
        r->check == ~(H2_FIXED_REQUEST_MAGIC ^ r->app_offset ^ r->app_size);
}
static inline int h2_fixed_request_valid(const h2_fixed_boot_request_t *r) {
    return r->magic == H2_FIXED_REQUEST_MAGIC && r->confirmed == H2_FIXED_ERASED &&
        h2_fixed_request_body_valid(r);
}
static inline int h2_fixed_request_consumed(const h2_fixed_boot_request_t *r) {
    return r->magic == 0u && h2_fixed_request_body_valid(r);
}
static inline int h2_fixed_request_confirmed(const h2_fixed_boot_request_t *r) {
    return h2_fixed_request_consumed(r) && r->confirmed == 0u;
}
static inline int h2_fixed_request_failed(const h2_fixed_boot_request_t *r) {
    return h2_fixed_request_consumed(r) && r->confirmed == H2_FIXED_ERASED;
}
/* Whether CP enters App on the next reset. */
static inline int h2_fixed_request_boots_app(const h2_fixed_boot_request_t *r) {
    return h2_fixed_request_valid(r) || h2_fixed_request_confirmed(r);
}
#endif
