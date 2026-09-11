#include <assert.h>
#include "layout.h"

#define KIB(n) ((uint32_t)(n) * 1024u)

typedef struct board {
    uint32_t loader_kib;
    uint32_t loader_cp_kib;
    uint32_t app_cp_kib;
} board_t;

/* Build the Loader and App partition tables of one board: Loader window right
 * after the 68 KiB bootloader, App window after it up to 0x75f000. */
static void tables(board_t b, h2_fixed_layout_t *loader, h2_fixed_layout_t *app) {
    const h2_fixed_window_t control = {0x77f000u, KIB(4)};
    const uint32_t loader_offset = KIB(68);
    const uint32_t app_offset = loader_offset + KIB(b.loader_kib);
    const uint32_t app_size = 0x75f000u - app_offset;
    assert(h2_fixed_layout_from_partitions(
        (h2_fixed_window_t){loader_offset, KIB(b.loader_cp_kib)},
        (h2_fixed_window_t){loader_offset + KIB(b.loader_cp_kib),
                            KIB(b.loader_kib - b.loader_cp_kib)},
        (h2_fixed_window_t){app_offset, app_size}, control, loader));
    assert(h2_fixed_layout_from_partitions(
        (h2_fixed_window_t){app_offset, KIB(b.app_cp_kib)},
        (h2_fixed_window_t){app_offset + KIB(b.app_cp_kib), app_size - KIB(b.app_cp_kib)},
        (h2_fixed_window_t){loader_offset, KIB(b.loader_kib)}, control, app));
}

static void test_board_owned_sizes(void) {
    /* The same code serves any board split, e.g. 2380, 1904 or 2584 KiB. */
    static const board_t boards[] = {{2380u, 1156u, 1156u}, {1904u, 952u, 1156u},
                                     {2584u, 1224u, 1360u}};
    for (unsigned i = 0; i < sizeof(boards) / sizeof(boards[0]); ++i) {
        h2_fixed_layout_t loader, app;
        tables(boards[i], &loader, &app);
        assert(!loader.app_table && app.app_table);
        assert(loader.loader.offset == app.loader.offset &&
               loader.loader.size == app.loader.size &&
               loader.loader.size == KIB(boards[i].loader_kib));
        assert(loader.app.offset == app.app.offset && loader.app.size == app.app.size);
        assert(loader.app.offset + loader.app.size == 0x75f000u);
        assert(loader.control_offset == 0x77f000u && app.control_offset == 0x77f000u);
    }
}

static void test_rejects_non_fixed_tables(void) {
    h2_fixed_layout_t out;
    const h2_fixed_window_t control = {0x77f000u, KIB(4)};
    /* CP and AP not contiguous. */
    assert(!h2_fixed_layout_from_partitions((h2_fixed_window_t){KIB(68), KIB(1156)},
                                            (h2_fixed_window_t){KIB(68) + KIB(1160), KIB(1224)},
                                            (h2_fixed_window_t){0x264000u, KIB(5100)}, control, &out));
    /* s_app overlapping the own window. */
    assert(!h2_fixed_layout_from_partitions((h2_fixed_window_t){KIB(68), KIB(1156)},
                                            (h2_fixed_window_t){KIB(68) + KIB(1156), KIB(1224)},
                                            (h2_fixed_window_t){0x200000u, KIB(5100)}, control, &out));
    /* Missing boot request partition. */
    assert(!h2_fixed_layout_from_partitions((h2_fixed_window_t){KIB(68), KIB(1156)},
                                            (h2_fixed_window_t){KIB(68) + KIB(1156), KIB(1224)},
                                            (h2_fixed_window_t){0x264000u, KIB(5100)},
                                            (h2_fixed_window_t){0u, 0u}, &out));
    /* Boot request inside an executable window. */
    assert(!h2_fixed_layout_from_partitions((h2_fixed_window_t){KIB(68), KIB(1156)},
                                            (h2_fixed_window_t){KIB(68) + KIB(1156), KIB(1224)},
                                            (h2_fixed_window_t){0x264000u, KIB(5100)},
                                            (h2_fixed_window_t){0x300000u, KIB(4)}, &out));
}

static void test_request_follows_layout(void) {
    h2_fixed_layout_t loader, app, other;
    tables((board_t){2380u, 1156u, 1156u}, &loader, &app);
    tables((board_t){1904u, 952u, 1156u}, &other, &app);
    h2_fixed_boot_request_t r = {H2_FIXED_REQUEST_MAGIC, loader.app.offset, loader.app.size,
                                 0, H2_FIXED_ERASED};
    r.check = ~(r.magic ^ r.app_offset ^ r.app_size);
    assert(h2_fixed_request_valid(&r, &loader) && h2_fixed_request_boots_app(&r, &loader));
    /* A record written for another board's App window does not boot here. */
    assert(!h2_fixed_request_valid(&r, &other));
    r.magic = 0;
    assert(h2_fixed_request_consumed(&r, &loader) && h2_fixed_request_failed(&r, &loader));
    assert(!h2_fixed_request_boots_app(&r, &loader));
    r.confirmed = 0u;
    assert(h2_fixed_request_confirmed(&r, &loader) && h2_fixed_request_boots_app(&r, &loader));
    r.check = 0xffffffffu;
    assert(!h2_fixed_request_boots_app(&r, &loader));
    assert(h2_fixed_xip_in_window(H2_FIXED_XIP_ADDRESS(loader.loader.offset) + 0x201u,
                                  &loader.loader));
    assert(!h2_fixed_xip_in_window(H2_FIXED_XIP_ADDRESS(loader.app.offset) + 0x201u,
                                   &loader.loader));
}

static void test_relay_head_offset(void) {
    const uint32_t head = 0x1100u;
    uint32_t offset = 0u;
    /* App window larger than the Loader image: copy to the window end. */
    assert(h2_fixed_relay_head_offset(KIB(5100), KIB(2380), head, &offset) == 1 &&
           offset == KIB(5100) - head);
    /* Smallest valid board gap, one 68 KiB block: no overlap. */
    assert(h2_fixed_relay_head_offset(KIB(2448), KIB(2380), head, &offset) == 1 &&
           offset == KIB(2448) - head && offset >= KIB(2380));
    /* Equal windows: the image's own head already ends the window. */
    assert(h2_fixed_relay_head_offset(KIB(2380), KIB(2380), head, &offset) == 0);
    /* A gap smaller than the head would overlap the image; too large an
     * image does not fit. */
    assert(h2_fixed_relay_head_offset(KIB(2380) + head - 1u, KIB(2380), head, &offset) == -1);
    assert(h2_fixed_relay_head_offset(KIB(2380), KIB(2448), head, &offset) == -1);
}

int main(void) {
    test_relay_head_offset();
    test_board_owned_sizes();
    test_rejects_non_fixed_tables();
    test_request_follows_layout();
    return 0;
}
