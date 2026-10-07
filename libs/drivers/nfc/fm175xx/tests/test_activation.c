#include "h2_fm175xx.h"
#include "h2_fm175xx_defs.h"

#include <assert.h>
#include <string.h>

typedef enum card_state { ABSENT, IDLE, READY1, READY2, ACTIVE, HALT } card_state_t;

typedef struct fixture {
    uint8_t registers[64];
    uint8_t tx[32];
    uint8_t rx[32];
    size_t tx_len;
    size_t rx_len;
    card_state_t card;
    uint8_t memory[160];
    unsigned halts;
    unsigned wakes;
    unsigned reads;
    int fail_register;
} fixture_t;

static void exchange(fixture_t *f) {
    f->registers[H2_FM175XX_REG_COMIRQ] = 1u;
    f->rx_len = 0u;
    if (f->tx_len == 2u && f->tx[0] == 0x50u && f->tx[1] == 0u) {
        assert(f->registers[H2_FM175XX_REG_TX_MODE] & 0x80u);
        ++f->halts;
        if (f->card == ACTIVE)
            f->card = HALT;
        /* ISO14443A HLTA deliberately has no reply. */
        return;
    }
    if (f->card == ABSENT)
        return;
    if (f->tx_len == 1u &&
        ((f->tx[0] == 0x26u && f->card == IDLE) ||
         (f->tx[0] == 0x52u && (f->card == IDLE || f->card == HALT)))) {
        ++f->wakes;
        f->card = READY1;
        f->rx[0] = 0x44u;
        f->rx[1] = 0u;
        f->rx_len = 2u;
    } else if (f->tx_len == 2u && f->tx[1] == 0x20u &&
               ((f->tx[0] == 0x93u && f->card == READY1) ||
                (f->tx[0] == 0x95u && f->card == READY2))) {
        const uint8_t cl1[5] = {0x88u, 4u, 1u, 2u, 0x8fu};
        const uint8_t cl2[5] = {3u, 4u, 5u, 6u, 4u};
        memcpy(f->rx, f->tx[0] == 0x93u ? cl1 : cl2, 5u);
        f->rx_len = 5u;
    } else if (f->tx_len == 7u && f->tx[1] == 0x70u &&
               ((f->tx[0] == 0x93u && f->card == READY1) ||
                (f->tx[0] == 0x95u && f->card == READY2))) {
        assert(f->registers[H2_FM175XX_REG_TX_MODE] & 0x80u);
        f->rx[0] = f->tx[0] == 0x93u ? 4u : 0u;
        f->rx_len = 1u;
        f->card = f->tx[0] == 0x93u ? READY2 : ACTIVE;
    } else if (f->tx_len == 2u && f->tx[0] == 0x30u && f->card == ACTIVE) {
        assert(f->registers[H2_FM175XX_REG_TX_MODE] & 0x80u);
        const size_t offset = (size_t)f->tx[1] * 4u;
        assert(offset + 16u <= sizeof(f->memory));
        memcpy(f->rx, f->memory + offset, 16u);
        f->rx_len = 16u;
        ++f->reads;
    } else {
        return;
    }
    f->registers[H2_FM175XX_REG_COMIRQ] = 0x20u;
}

static int write_register(void *user, uint8_t reg, uint8_t value) {
    fixture_t *f = user;
    if (f->fail_register)
        return H2_FM175XX_ERR_IO;
    if (reg == H2_FM175XX_REG_COMIRQ) {
        f->registers[reg] &= (uint8_t)~value;
    } else if (reg == H2_FM175XX_REG_FIFO_LEVEL && (value & 0x80u)) {
        f->tx_len = 0u;
        f->rx_len = 0u;
        f->registers[reg] = 0u;
    } else if (reg == H2_FM175XX_REG_CONTROL) {
        /* RxLastBits remains zero for these byte-aligned replies. */
        f->registers[reg] = value & 0xf8u;
    } else {
        f->registers[reg] = value;
    }
    if (reg == H2_FM175XX_REG_COMMAND && value == H2_FM175XX_CMD_TRANSCEIVE)
        f->registers[H2_FM175XX_REG_COMIRQ] = 4u;
    if (reg == H2_FM175XX_REG_BIT_FRAMING && (value & 0x80u) &&
        f->registers[H2_FM175XX_REG_COMMAND] == H2_FM175XX_CMD_TRANSCEIVE)
        exchange(f);
    return H2_FM175XX_OK;
}

static int write_registers(void *user, uint8_t reg, const uint8_t *data, size_t len) {
    fixture_t *f = user;
    assert(reg == H2_FM175XX_REG_FIFO_DATA && len <= sizeof(f->tx));
    memcpy(f->tx, data, len);
    f->tx_len = len;
    return H2_FM175XX_OK;
}

static int read_register(void *user, uint8_t reg, uint8_t *out) {
    fixture_t *f = user;
    *out = reg == H2_FM175XX_REG_FIFO_LEVEL ? (uint8_t)f->rx_len : f->registers[reg];
    return H2_FM175XX_OK;
}

static int read_registers(void *user, uint8_t reg, uint8_t *out, size_t len) {
    fixture_t *f = user;
    assert(reg == H2_FM175XX_REG_FIFO_DATA && len == f->rx_len);
    memcpy(out, f->rx, len);
    f->rx_len = 0u;
    return H2_FM175XX_OK;
}

static void sleep_ms(void *user, uint32_t ms) { (void)user; (void)ms; }

int main(void) {
    fixture_t f = {.card = IDLE};
    f.memory[12] = 0xe1u;
    f.memory[13] = 0x10u;
    f.memory[14] = 18u;
    f.memory[16] = 3u;
    f.memory[17] = 1u;
    f.memory[18] = 0xfeu;
    const h2_fm175xx_transport_t transport = {
        .user = &f, .write_reg = write_register, .write_regs = write_registers,
        .read_reg = read_register, .read_regs = read_registers, .sleep_ms = sleep_ms,
    };
    h2_fm175xx_t reader;
    assert(h2_fm175xx_init(&reader, &transport) == H2_FM175XX_OK);
    assert(h2_fm175xx_open_type_a(&reader) == H2_FM175XX_OK);
    h2_fm175xx_type_a_card_t first, next;
    assert(h2_fm175xx_type_a_activate(&reader, &first) == H2_FM175XX_OK);
    assert(f.card == ACTIVE);
    /* A Runtime scan has already selected the card. Repeating activation
     * before a content read must not report that same card as absent. */
    assert(h2_fm175xx_type_a_activate(&reader, &next) == H2_FM175XX_OK);
    assert(memcmp(&first, &next, sizeof(first)) == 0);
    uint8_t pages[256];
    size_t len = 0u;
    assert(h2_fm175xx_ntag_read_all(&reader, pages, sizeof(pages), &len) == H2_FM175XX_OK);
    assert(len == sizeof(f.memory) && memcmp(pages, f.memory, len) == 0);
    assert(f.reads == 10u);
    for (unsigned i = 0u; i < 3u; ++i)
        assert(h2_fm175xx_type_a_activate(&reader, &next) == H2_FM175XX_OK);
    f.card = HALT;
    assert(h2_fm175xx_type_a_activate(&reader, &next) == H2_FM175XX_OK);
    f.card = ABSENT;
    assert(h2_fm175xx_type_a_activate(&reader, &next) == H2_FM175XX_ERR_TIMEOUT);
    f.fail_register = 1;
    assert(h2_fm175xx_type_a_activate(&reader, &next) == H2_FM175XX_ERR_IO);
    return 0;
}
