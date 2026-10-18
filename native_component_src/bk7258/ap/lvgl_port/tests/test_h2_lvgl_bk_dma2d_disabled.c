#include "h2_lvgl_bk_dma2d.h"
#include "h2_lvgl_bk_power.h"
#include <assert.h>
#include <stddef.h>
void __wrap_lv_deinit(void);
static unsigned real_deinit_calls;
void __real_lv_deinit(void) { ++real_deinit_calls; }
int main(void) {
    uint16_t pixel = 0x1234;
    assert(h2_bk_dma2d_rgb565(&pixel, NULL, 16, 8, 32, 0, 0xabcd) == 0);
    assert(pixel == 0x1234);
    assert(h2_bk_dma2d_suspend() == 0);
    __wrap_lv_deinit();
    __wrap_lv_deinit();
    assert(real_deinit_calls == 2);
    return 0;
}
