#include "layout.h"
#include "armstar.h"
#include "driver/flash.h"
#include "driver/flash_partition.h"
#include "os/os.h"

extern void stop_cpu1_core(void);
extern void stop_cpu2_core(void);

/* Enter the new image with reset-like CPU state. This function must not use
 * the old task stack after switching CONTROL/MSP. Both images remain in Flash.
 */
__attribute__((naked, noreturn)) static void enter_image(uint32_t msp, uint32_t pc) {
    __asm__ volatile(
        "movs r2, #0\n"
        "msr msplim, r2\n"
        "msr psplim, r2\n"
        "msr control, r2\n"
        "isb\n"
        "msr msp, r0\n"
        "bx r1\n");
}

static h2_fixed_window_t partition_window(bk_partition_t id) {
    const bk_logic_partition_t *p = bk_flash_partition_get_info(id);
    return p ? (h2_fixed_window_t){p->partition_start_addr, p->partition_length}
             : (h2_fixed_window_t){0u, 0u};
}

void h2loader_cp_try_fixed_app(void) {
#ifdef BK_PARTITION_H2_BOOT_REQUEST
    h2_fixed_layout_t layout;
    h2_fixed_boot_request_t request;
    uint32_t consumed = 0;
    /* Only the Loader image hands off; the board's partition table gives both
     * windows and the boot record. */
    if (!h2_fixed_layout_from_partitions(
            partition_window(BK_PARTITION_APPLICATION),
            partition_window(BK_PARTITION_APPLICATION1),
            partition_window(BK_PARTITION_S_APP),
            partition_window(BK_PARTITION_H2_BOOT_REQUEST), &layout) ||
        layout.app_table) return;
    if (bk_flash_read_bytes(layout.control_offset, (uint8_t *)&request,
                           sizeof(request)) != BK_OK ||
        !h2_fixed_request_boots_app(&request, &layout)) return;
    /* Clear magic before validating or executing App. For a trial this
     * consumes the request, so an unconfirmed reset or a rejected vector
     * returns to Loader as a failed attempt through the unchanged native A
     * boot selection. For a confirmed App the bits are already zero and the
     * write changes nothing, but it must still run: on hardware, entering App
     * without a preceding Flash program operation faults in App CP startup
     * (HardFault, pc=0 from bk_pm_module_vote_power_ctrl). */
    flash_protect_type_t protect = bk_flash_get_protect_type();
    if (bk_flash_set_protect_type(FLASH_PROTECT_NONE) != BK_OK) return;
    int rc = bk_flash_write_bytes(layout.control_offset, (uint8_t *)&consumed, sizeof(consumed));
    (void)bk_flash_set_protect_type(protect);
    if (rc != BK_OK) return;
    const volatile uint32_t *vector = (const volatile uint32_t *)
        H2_FIXED_XIP_ADDRESS(layout.app.offset);
    uint32_t msp = vector[0], pc = vector[1];
    if (msp < 0x28000000u || msp > 0x280a0000u || (msp & 7u) ||
        !(pc & 1u) || !h2_fixed_xip_in_window(pc, &layout.app)) return;
    stop_cpu2_core();
    stop_cpu1_core();
    __disable_irq();
    SysTick->CTRL = 0;
    for (unsigned i = 0; i < 8; ++i) {
        NVIC->ICER[i] = 0xffffffffu;
        NVIC->ICPR[i] = 0xffffffffu;
    }
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;
    /* App's reset handler copies initialized data through SRAM's code alias.
     * Loader's running MPU marks that alias read-only. Restore reset state
     * before the copy; App installs its own protection during SDK startup. */
    SCB_DisableDCache();
    SCB_DisableICache();
    ARM_MPU_Disable();
    SCB->VTOR = (uint32_t)vector;
    __DSB();
    __ISB();
    enter_image(msp, pc);
#endif
}
