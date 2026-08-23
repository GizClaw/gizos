#include "h2_fm175xx.h"

#include "h2_fm175xx_defs.h"

#include <string.h>

static int write_reg(h2_fm175xx_t *reader, uint8_t reg, uint8_t value) {
    return reader->transport.write_reg(reader->transport.user, reg, value);
}

static int write_regs(h2_fm175xx_t *reader, uint8_t reg, const uint8_t *data, size_t len) {
    if (len == 0u) {
        return H2_FM175XX_OK;
    }
    return reader->transport.write_regs(reader->transport.user, reg, data, len);
}

static int read_reg(h2_fm175xx_t *reader, uint8_t reg, uint8_t *out_value) {
    return reader->transport.read_reg(reader->transport.user, reg, out_value);
}

static int read_regs(h2_fm175xx_t *reader, uint8_t reg, uint8_t *out_data, size_t len) {
    if (len == 0u) {
        return H2_FM175XX_OK;
    }
    return reader->transport.read_regs(reader->transport.user, reg, out_data, len);
}

static void sleep_ms(h2_fm175xx_t *reader, uint32_t ms) {
    if (reader->transport.sleep_ms != NULL) {
        reader->transport.sleep_ms(reader->transport.user, ms);
    }
}

static int set_bit_mask(h2_fm175xx_t *reader, uint8_t reg, uint8_t mask) {
    uint8_t value = 0;
    int rc = read_reg(reader, reg, &value);
    if (rc != H2_FM175XX_OK) {
        return rc;
    }
    return write_reg(reader, reg, (uint8_t)(value | mask));
}

static int clear_bit_mask(h2_fm175xx_t *reader, uint8_t reg, uint8_t mask) {
    uint8_t value = 0;
    int rc = read_reg(reader, reg, &value);
    if (rc != H2_FM175XX_OK) {
        return rc;
    }
    return write_reg(reader, reg, (uint8_t)(value & (uint8_t)~mask));
}

static int write_fifo(h2_fm175xx_t *reader, const uint8_t *data, size_t len) {
    return write_regs(reader, H2_FM175XX_REG_FIFO_DATA, data, len);
}

static int read_fifo(h2_fm175xx_t *reader, uint8_t *data, size_t len) {
    return read_regs(reader, H2_FM175XX_REG_FIFO_DATA, data, len);
}

static int clear_fifo(h2_fm175xx_t *reader) {
    int rc = set_bit_mask(reader, H2_FM175XX_REG_FIFO_LEVEL, 0x80u);
    if (rc != H2_FM175XX_OK) {
        return rc;
    }
    uint8_t level = 0;
    rc = read_reg(reader, H2_FM175XX_REG_FIFO_LEVEL, &level);
    if (rc != H2_FM175XX_OK) {
        return rc;
    }
    return level == 0u ? H2_FM175XX_OK : H2_FM175XX_ERR_PROTOCOL;
}

static int set_timer(h2_fm175xx_t *reader, uint32_t timeout_ms) {
    uint16_t prescaler = 1u;
    uint32_t reload = 0xffffu;
    while (prescaler < 0x0fffu) {
        reload = ((timeout_ms * 13560u) - 1u) / ((prescaler * 2u) + 1u);
        if (reload < 0x10000u) {
            break;
        }
        prescaler++;
    }

    int rc = set_bit_mask(reader, H2_FM175XX_REG_TMODE, (uint8_t)(prescaler >> 8));
    if (rc == H2_FM175XX_OK) {
        rc = write_reg(reader, H2_FM175XX_REG_TPRESCALER, (uint8_t)(prescaler & 0xffu));
    }
    if (rc == H2_FM175XX_OK) {
        rc = write_reg(reader, H2_FM175XX_REG_TRELOAD_HI, (uint8_t)((reload >> 8) & 0xffu));
    }
    if (rc == H2_FM175XX_OK) {
        rc = write_reg(reader, H2_FM175XX_REG_TRELOAD_LO, (uint8_t)(reload & 0xffu));
    }
    return rc;
}

static int pre_cmd(h2_fm175xx_t *reader) {
    int rc = clear_fifo(reader);
    if (rc == H2_FM175XX_OK) {
        rc = write_reg(reader, H2_FM175XX_REG_COMMAND, H2_FM175XX_CMD_IDLE);
    }
    if (rc == H2_FM175XX_OK) {
        rc = write_reg(reader, H2_FM175XX_REG_WATER_LEVEL, 0x20u);
    }
    if (rc == H2_FM175XX_OK) {
        rc = write_reg(reader, H2_FM175XX_REG_COMIRQ, 0x7fu);
    }
    return rc;
}

static int post_cmd(h2_fm175xx_t *reader, int rc) {
    if (rc == H2_FM175XX_OK) {
        uint8_t error = 0;
        rc = read_reg(reader, H2_FM175XX_REG_ERROR, &error);
        if (rc == H2_FM175XX_OK && error != 0u) {
            rc = H2_FM175XX_ERR_PROTOCOL;
        }
    }
    (void)set_bit_mask(reader, H2_FM175XX_REG_CONTROL, 0x80u);
    (void)write_reg(reader, H2_FM175XX_REG_COMMAND, H2_FM175XX_CMD_IDLE);
    (void)set_bit_mask(reader, H2_FM175XX_REG_BIT_FRAMING, 0x00u);
    return rc;
}

static int transceive(
    h2_fm175xx_t *reader,
    const uint8_t *in,
    size_t in_len,
    uint8_t *out,
    size_t out_cap,
    size_t *out_bits) {
    if ((in_len > 0u && in == NULL) || out == NULL || out_bits == NULL) {
        return H2_FM175XX_ERR_INVALID_ARG;
    }

    int rc = pre_cmd(reader);
    if (rc != H2_FM175XX_OK) {
        return rc;
    }
    rc = set_bit_mask(reader, H2_FM175XX_REG_TMODE, 0x80u);
    if (rc == H2_FM175XX_OK) {
        rc = write_reg(reader, H2_FM175XX_REG_COMMAND, H2_FM175XX_CMD_TRANSCEIVE);
    }
    if (rc != H2_FM175XX_OK) {
        return post_cmd(reader, rc);
    }

    size_t out_len = 0u;
    for (uint32_t guard = 0; guard < 100u; ++guard) {
        uint8_t irq = 0;
        rc = read_reg(reader, H2_FM175XX_REG_COMIRQ, &irq);
        if (rc != H2_FM175XX_OK) {
            return post_cmd(reader, rc);
        }
        if ((irq & 0x01u) != 0u) {
            return post_cmd(reader, H2_FM175XX_ERR_TIMEOUT);
        }
        if ((irq & 0x04u) != 0u && in_len > 0u) {
            const size_t chunk = in_len > 32u ? 32u : in_len;
            rc = write_fifo(reader, in, chunk);
            if (rc != H2_FM175XX_OK) {
                return post_cmd(reader, rc);
            }
            in += chunk;
            in_len -= chunk;
            rc = set_bit_mask(reader, H2_FM175XX_REG_BIT_FRAMING, 0x80u);
            if (rc == H2_FM175XX_OK) {
                rc = write_reg(reader, H2_FM175XX_REG_COMIRQ, 0x04u);
            }
            if (rc != H2_FM175XX_OK) {
                return post_cmd(reader, rc);
            }
        }
        if ((irq & 0x08u) != 0u && (irq & 0x40u) != 0u && in_len == 0u) {
            uint8_t fifo_level = 0;
            rc = read_reg(reader, H2_FM175XX_REG_FIFO_LEVEL, &fifo_level);
            if (rc != H2_FM175XX_OK) {
                return post_cmd(reader, rc);
            }
            if (fifo_level > 32u) {
                if (out_len + 32u > out_cap) {
                    return post_cmd(reader, H2_FM175XX_ERR_PROTOCOL);
                }
                rc = read_fifo(reader, out + out_len, 32u);
                if (rc != H2_FM175XX_OK) {
                    return post_cmd(reader, rc);
                }
                out_len += 32u;
                rc = write_reg(reader, H2_FM175XX_REG_COMIRQ, 0x08u);
                if (rc != H2_FM175XX_OK) {
                    return post_cmd(reader, rc);
                }
            }
        }
        if ((irq & 0x20u) != 0u && in_len == 0u) {
            uint8_t fifo_level = 0;
            uint8_t last_bits = 0;
            rc = read_reg(reader, H2_FM175XX_REG_FIFO_LEVEL, &fifo_level);
            if (rc == H2_FM175XX_OK) {
                rc = read_reg(reader, H2_FM175XX_REG_CONTROL, &last_bits);
            }
            if (rc != H2_FM175XX_OK) {
                return post_cmd(reader, rc);
            }
            last_bits &= 0x07u;
            if (fifo_level == 0u && last_bits > 0u) {
                fifo_level = 1u;
            }
            if (out_len + fifo_level > out_cap) {
                return post_cmd(reader, H2_FM175XX_ERR_PROTOCOL);
            }
            rc = read_fifo(reader, out + out_len, fifo_level);
            if (rc != H2_FM175XX_OK) {
                return post_cmd(reader, rc);
            }
            out_len += fifo_level;
            *out_bits = last_bits > 0u ? ((out_len - 1u) * 8u) + last_bits : out_len * 8u;
            return post_cmd(reader, H2_FM175XX_OK);
        }
        sleep_ms(reader, 1u);
    }
    return post_cmd(reader, H2_FM175XX_ERR_TIMEOUT);
}

static int type_a_request(h2_fm175xx_t *reader, uint8_t out_atqa[2]) {
    int rc = clear_bit_mask(reader, H2_FM175XX_REG_TX_MODE, 0x80u);
    if (rc == H2_FM175XX_OK) {
        rc = clear_bit_mask(reader, H2_FM175XX_REG_RX_MODE, 0x80u);
    }
    if (rc == H2_FM175XX_OK) {
        rc = clear_bit_mask(reader, H2_FM175XX_REG_STATUS2, 0x08u);
    }
    if (rc == H2_FM175XX_OK) {
        rc = write_reg(reader, H2_FM175XX_REG_BIT_FRAMING, 0x07u);
    }
    if (rc == H2_FM175XX_OK) {
        rc = set_timer(reader, 1u);
    }
    if (rc != H2_FM175XX_OK) {
        return rc;
    }
    const uint8_t request = 0x26u;
    size_t bits = 0u;
    rc = transceive(reader, &request, 1u, out_atqa, 2u, &bits);
    if (rc != H2_FM175XX_OK) {
        return rc;
    }
    return bits == 16u ? H2_FM175XX_OK : H2_FM175XX_ERR_PROTOCOL;
}

static int type_a_anticollision(h2_fm175xx_t *reader, uint8_t selcode, uint8_t out_uid[5]) {
    int rc = clear_bit_mask(reader, H2_FM175XX_REG_TX_MODE, 0x80u);
    if (rc == H2_FM175XX_OK) {
        rc = clear_bit_mask(reader, H2_FM175XX_REG_RX_MODE, 0x80u);
    }
    if (rc == H2_FM175XX_OK) {
        rc = clear_bit_mask(reader, H2_FM175XX_REG_STATUS2, 0x08u);
    }
    if (rc == H2_FM175XX_OK) {
        rc = write_reg(reader, H2_FM175XX_REG_BIT_FRAMING, 0x00u);
    }
    if (rc == H2_FM175XX_OK) {
        rc = write_reg(reader, H2_FM175XX_REG_COLL, 0x80u);
    }
    if (rc == H2_FM175XX_OK) {
        rc = set_timer(reader, 1u);
    }
    if (rc != H2_FM175XX_OK) {
        return rc;
    }
    const uint8_t send[2] = { selcode, 0x20u };
    uint8_t recv[5] = {0};
    size_t bits = 0u;
    rc = transceive(reader, send, sizeof(send), recv, sizeof(recv), &bits);
    if (rc != H2_FM175XX_OK) {
        return rc;
    }
    if (bits != 40u || recv[4] != (uint8_t)(recv[0] ^ recv[1] ^ recv[2] ^ recv[3])) {
        return H2_FM175XX_ERR_PROTOCOL;
    }
    memcpy(out_uid, recv, sizeof(recv));
    return H2_FM175XX_OK;
}

static int type_a_select(h2_fm175xx_t *reader, uint8_t selcode, const uint8_t uid[5], uint8_t *out_sak) {
    uint8_t send[7] = { selcode, 0x70u, uid[0], uid[1], uid[2], uid[3], uid[4] };
    uint8_t recv[5] = {0};
    size_t bits = 0u;
    int rc = write_reg(reader, H2_FM175XX_REG_BIT_FRAMING, 0x00u);
    if (rc == H2_FM175XX_OK) {
        rc = set_bit_mask(reader, H2_FM175XX_REG_TX_MODE, 0x80u);
    }
    if (rc == H2_FM175XX_OK) {
        rc = set_bit_mask(reader, H2_FM175XX_REG_RX_MODE, 0x80u);
    }
    if (rc == H2_FM175XX_OK) {
        rc = clear_bit_mask(reader, H2_FM175XX_REG_STATUS2, 0x08u);
    }
    if (rc == H2_FM175XX_OK) {
        rc = set_timer(reader, 1u);
    }
    if (rc == H2_FM175XX_OK) {
        rc = transceive(reader, send, sizeof(send), recv, sizeof(recv), &bits);
    }
    if (rc != H2_FM175XX_OK) {
        return rc;
    }
    *out_sak = recv[0];
    return H2_FM175XX_OK;
}

static int ntag_read_once(h2_fm175xx_t *reader, uint8_t page_addr, uint8_t out[16]) {
    const uint8_t send[2] = { 0x30u, page_addr };
    size_t bits = 0u;
    int rc = set_timer(reader, 5u);
    if (rc == H2_FM175XX_OK) {
        rc = transceive(reader, send, sizeof(send), out, 16u, &bits);
    }
    if (rc != H2_FM175XX_OK) {
        return rc;
    }
    return bits == 128u ? H2_FM175XX_OK : H2_FM175XX_ERR_PROTOCOL;
}

int h2_fm175xx_init(h2_fm175xx_t *reader, const h2_fm175xx_transport_t *transport) {
    if (reader == NULL || transport == NULL ||
        transport->write_reg == NULL ||
        transport->write_regs == NULL ||
        transport->read_reg == NULL ||
        transport->read_regs == NULL) {
        return H2_FM175XX_ERR_INVALID_ARG;
    }
    reader->transport = *transport;
    return H2_FM175XX_OK;
}

int h2_fm175xx_open_type_a(h2_fm175xx_t *reader) {
    if (reader == NULL) {
        return H2_FM175XX_ERR_INVALID_ARG;
    }
    int rc = write_reg(reader, H2_FM175XX_REG_COMMAND, H2_FM175XX_CMD_SOFT_RESET);
    sleep_ms(reader, 2u);
    if (rc == H2_FM175XX_OK) {
        rc = set_bit_mask(reader, H2_FM175XX_REG_CONTROL, 0x10u);
    }
    if (rc == H2_FM175XX_OK) {
        rc = set_bit_mask(reader, H2_FM175XX_REG_TX_AUTO, 0x40u);
    }
    if (rc == H2_FM175XX_OK) {
        rc = write_reg(reader, H2_FM175XX_REG_TX_MODE, 0x00u);
    }
    if (rc == H2_FM175XX_OK) {
        rc = write_reg(reader, H2_FM175XX_REG_RX_MODE, 0x00u);
    }
    if (rc == H2_FM175XX_OK) {
        rc = write_reg(reader, H2_FM175XX_REG_GSN, 0xf8u);
    }
    if (rc == H2_FM175XX_OK) {
        rc = write_reg(reader, H2_FM175XX_REG_GWGSP, 0x3fu);
    }
    if (rc == H2_FM175XX_OK) {
        rc = write_reg(reader, H2_FM175XX_REG_RFCFG, 0x68u);
    }
    if (rc == H2_FM175XX_OK) {
        rc = write_reg(reader, H2_FM175XX_REG_RX_THRES, 0x74u);
    }
    if (rc == H2_FM175XX_OK) {
        rc = write_reg(reader, H2_FM175XX_REG_TX_CTRL, 0x83u);
    }
    return rc;
}

int h2_fm175xx_type_a_activate(h2_fm175xx_t *reader, h2_fm175xx_type_a_card_t *out_card) {
    if (reader == NULL || out_card == NULL) {
        return H2_FM175XX_ERR_INVALID_ARG;
    }
    memset(out_card, 0, sizeof(*out_card));
    int rc = type_a_request(reader, out_card->atqa);
    if (rc != H2_FM175XX_OK) {
        return rc;
    }

    const uint8_t cascade = (uint8_t)(out_card->atqa[0] & 0xc0u);
    const uint8_t levels = cascade == 0x00u ? 1u : (cascade == 0x40u ? 2u : (cascade == 0x80u ? 3u : 0u));
    if (levels == 0u) {
        return H2_FM175XX_ERR_PROTOCOL;
    }

    static const uint8_t selcodes[3] = { 0x93u, 0x95u, 0x97u };
    for (uint8_t i = 0u; i < levels; ++i) {
        uint8_t *uid = out_card->uid + (i * 5u);
        rc = type_a_anticollision(reader, selcodes[i], uid);
        if (rc != H2_FM175XX_OK) {
            return rc;
        }
        rc = type_a_select(reader, selcodes[i], uid, &out_card->sak[i]);
        if (rc != H2_FM175XX_OK) {
            return rc;
        }
    }
    out_card->uid_len = (uint8_t)(levels * 5u);
    out_card->sak_len = levels;
    return H2_FM175XX_OK;
}

int h2_fm175xx_ntag_read_all(h2_fm175xx_t *reader, uint8_t *out_data, size_t capacity, size_t *out_len) {
    if (reader == NULL || out_data == NULL || out_len == NULL || capacity < 16u) {
        return H2_FM175XX_ERR_INVALID_ARG;
    }
    int rc = ntag_read_once(reader, 0u, out_data);
    if (rc != H2_FM175XX_OK) {
        return rc;
    }
    const size_t total_len = 16u + (8u * out_data[14]);
    if (total_len > capacity) {
        return H2_FM175XX_ERR_NO_MEMORY;
    }
    for (size_t offset = 16u; offset < total_len; offset += 16u) {
        uint8_t page[16];
        rc = ntag_read_once(reader, (uint8_t)(offset / 4u), page);
        if (rc != H2_FM175XX_OK) {
            return rc;
        }
        const size_t remaining = total_len - offset;
        memcpy(out_data + offset, page, remaining < sizeof(page) ? remaining : sizeof(page));
    }
    *out_len = total_len;
    return H2_FM175XX_OK;
}
