#include "h2_lvgl_bk_power.h"

void __real_lv_deinit(void);
void __wrap_lv_deinit(void);

/* SDK-only linker integration: upstream joins its draw workers before target
 * interrupts, power and completion storage can be released. */
void __wrap_lv_deinit(void) {
    __real_lv_deinit();
    (void)h2_bk_dma2d_suspend();
}
