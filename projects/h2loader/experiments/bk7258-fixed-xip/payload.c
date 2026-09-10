/* A separately linked freestanding program: no SDK or Loader symbols.
 * Execute briefly from the other Flash address, then return for bench access.
 * This does not claim a full CP/AP SDK image handoff.
 */
#include <stdint.h>

static const volatile uint32_t identity = 0x72585849u;

static void putc_raw(char c) {
    volatile uint32_t *status = (volatile uint32_t *)0x44820018u;
    volatile uint32_t *fifo = (volatile uint32_t *)0x4482001cu;
    while (*status & (1u << 16)) {}
    *fifo = (uint32_t)c;
}

static void puts_raw(const char *s) {
    while (*s) putc_raw(*s++);
}

static void hex_raw(uint32_t x) {
    for (int shift = 28; shift >= 0; shift -= 4)
        putc_raw("0123456789abcdef"[(x >> shift) & 15]);
}

__attribute__((section(".entry"), used))
void payload_entry(uint32_t *result) {
    uint32_t pc;
    __asm__ volatile("mov %0, pc" : "=r"(pc));
    result[0] = pc;
    result[1] = (uint32_t)&identity;
    result[2] = identity;
    for (uint32_t i = 0; i < 3; ++i) {
        puts_raw("H2_FIXED_XIP_PAYLOAD pc=");
        hex_raw(pc);
        puts_raw(" rodata=");
        hex_raw((uint32_t)&identity);
        puts_raw(" value=");
        hex_raw(identity);
        puts_raw("\r\n");
        result[3] = i + 1;
    }
}
