#include "app_config.h"
#include "system/includes.h"
#include "asm/clock.h"
#include "asm/irq.h"
#include "system/spinlock.h"
#include "h2_jieli_ac791n_clock_math.h"
#include "h2/pal/core/h2_pal_errors.h"

#ifdef H2_JIELI_CLOCK_TIMER5

/* TIMER4 belongs to sys_usec_timer. The shared layout reserves TIMER5 for
 * PAL monotonic time, using the same oscillator input selection as the SDK
 * TIMER1 driver (CON bit 3). No PWM/capture user may claim TIMER5 here.
 * At 24 MHz resolution is 341.33 us and wrap interval is 22.37 s; even at
 * 48 MHz it exceeds the layout's 4-second watchdog. IRQ blackout must never
 * span two wraps. System sleep requires a future retained-clock backend. */
#if (TCFG_LOWPOWER_LOWPOWER_SEL & (SYS_SLEEP_EN | RF_FORCE_SYS_SLEEP_EN)) || TCFG_LOWPOWER_BTOSC_DISABLE
#error "TIMER5 monotonic clock requires the oscillator to remain running"
#endif

static DEFINE_SPINLOCK(clock_lock);
static uint64_t completed_ticks;
static uint32_t oscillator_hz;

static ___interrupt SEC_USED(.volatile_ram_code) void clock_overflow(void)
{
    unsigned flags;
    local_irq_save(flags);
    spin_acquire(&clock_lock);
    /* SDK csync orders the CPU, but has no compiler memory clobber. */
    __asm__ volatile("" ::: "memory");
    if (JL_TIMER5->CON & BIT(15)) {
        completed_ticks += H2_JIELI_CLOCK_PERIOD;
        JL_TIMER5->CON |= BIT(14);
    }
    __asm__ volatile("" ::: "memory");
    spin_release(&clock_lock);
    local_irq_restore(flags);
}

int h2_jieli_ac791n_devkit_clock_read_us(uint64_t *out_us)
{
    if (out_us == NULL) return H2_PAL_ERR_INVALID_ARG;
    unsigned flags;
    local_irq_save(flags);
    spin_acquire(&clock_lock);
    __asm__ volatile("" ::: "memory");
    if (oscillator_hz == 0u) {
        const int hz = clk_get("osc");
        if (hz < 12000000 || hz > 48000000 || (JL_TIMER5->CON & 3u)) {
            __asm__ volatile("" ::: "memory");
            spin_release(&clock_lock);
            local_irq_restore(flags);
            return H2_PAL_ERR_UNAVAILABLE;
        }
        JL_TIMER5->CON = BIT(14);
        JL_TIMER5->CNT = 0;
        JL_TIMER5->PRD = H2_JIELI_CLOCK_PERIOD;
        request_irq(IRQ_TIMER5_IDX, 1, clock_overflow, 0);
        oscillator_hz = (uint32_t)hz;
        /* SDK prescaler table maps /8192 to CON[7:4] = 14. */
        JL_TIMER5->CON = (14u << 4) | BIT(3) | BIT(0);
    }
    const unsigned pending_before = JL_TIMER5->CON & BIT(15);
    uint32_t counter = JL_TIMER5->CNT;
    const unsigned pending_after = JL_TIMER5->CON & BIT(15);
    if (pending_before != pending_after) counter = JL_TIMER5->CNT;
    const uint64_t ticks = h2_jieli_clock_snapshot_ticks(
        completed_ticks, counter, pending_after != 0u);
    const uint32_t hz = oscillator_hz;
    __asm__ volatile("" ::: "memory");
    spin_release(&clock_lock);
    local_irq_restore(flags);
    *out_us = h2_jieli_clock_ticks_to_us(ticks, hz);
    return H2_PAL_OK;
}

#endif
