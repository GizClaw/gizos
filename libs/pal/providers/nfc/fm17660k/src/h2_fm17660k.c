#include "h2_fm17660k.h"

#include "h2_fm17660k_internal.h"
#include "h2_nfc_type2.h"

#include <string.h>

#define FM17660K_EVENT_QUEUE_SIZE 12u
#define FM17660K_CARD_FRAME_MAX_SIZE 255u
#define FM17660K_DEFAULT_TIMEOUT_MS 52u
#define FM17660K_FIFO_FLUSH_SETTLE_MS 2u
#define FM17660K_READER_RX_GRACE_MS 12u
#define FM17660K_IDLE_POLL_LIMIT 64u
#define FM17660K_CARD_POLL_PROGRESS_LIMIT 64u
#define FM17660K_RAW_RESPONSE_DEADLINE_MS 25u

typedef enum fm17660k_frontend_owner {
    FM17660K_FRONTEND_NONE = 0,
    FM17660K_FRONTEND_READER,
    FM17660K_FRONTEND_CARD_EMULATION,
} fm17660k_frontend_owner_t;

typedef enum fm17660k_card_poll_phase {
    FM17660K_CARD_POLL_IDLE = 0,
    FM17660K_CARD_POLL_IRQ_ENTER_READ,
    FM17660K_CARD_POLL_IRQ_ENTER_WRITE,
    FM17660K_CARD_POLL_IRQ_READ_MAIN,
    FM17660K_CARD_POLL_IRQ_READ_AUX,
    FM17660K_CARD_POLL_IRQ_ACK_MAIN,
    FM17660K_CARD_POLL_IRQ_ACK_AUX,
    FM17660K_CARD_POLL_IRQ_LEAVE_READ,
    FM17660K_CARD_POLL_IRQ_LEAVE_WRITE,
    FM17660K_CARD_POLL_PROCESS,
    FM17660K_CARD_POLL_RX_LENGTH,
    FM17660K_CARD_POLL_RX_FIFO,
    FM17660K_CARD_POLL_RX_LAST_BITS,
    FM17660K_CARD_POLL_PREPARE_RESPONSE,
    FM17660K_CARD_POLL_TX_FLUSH_READ,
    FM17660K_CARD_POLL_TX_FLUSH_WRITE,
    FM17660K_CARD_POLL_TX_FIFO,
    FM17660K_CARD_POLL_TX_ENTER_READ,
    FM17660K_CARD_POLL_TX_ENTER_WRITE,
    FM17660K_CARD_POLL_TX_COMMAND,
    FM17660K_CARD_POLL_TX_LEAVE_READ,
    FM17660K_CARD_POLL_TX_LEAVE_WRITE,
    FM17660K_CARD_POLL_POSTPROCESS,
    FM17660K_CARD_POLL_REARM_FLUSH_READ,
    FM17660K_CARD_POLL_REARM_FLUSH_WRITE,
    FM17660K_CARD_POLL_REARM_ENTER_READ,
    FM17660K_CARD_POLL_REARM_ENTER_WRITE,
    FM17660K_CARD_POLL_REARM_MAIN_IRQ,
    FM17660K_CARD_POLL_REARM_AUX_IRQ,
    FM17660K_CARD_POLL_REARM_NC_MODE,
    FM17660K_CARD_POLL_REARM_RF_COMMAND,
    FM17660K_CARD_POLL_REARM_LEAVE_READ,
    FM17660K_CARD_POLL_REARM_LEAVE_WRITE,
    FM17660K_CARD_POLL_COMPLETE,
} fm17660k_card_poll_phase_t;

typedef struct fm17660k_card_open_config {
    h2_pal_periph_id_t periph_id;
    h2_pal_nfc_card_emulation_technology_t technology;
    h2_pal_nfc_card_emulation_mode_t mode;
    h2_pal_nfc_card_emulation_profile_t managed_profile;
    const uint8_t *uid;
    uint8_t uid_len;
} fm17660k_card_open_config_t;

typedef struct fm17660k_card_content {
    h2_pal_nfc_card_emulation_mode_t mode;
    uint32_t revision;
    const uint8_t *managed_bytes;
    size_t managed_len;
    h2_pal_nfc_card_emulation_raw_exchange_fn raw_exchange;
    void *raw_exchange_user;
} fm17660k_card_content_t;

typedef enum fm17660k_card_event_type {
    H2_PAL_NFC_CARD_EMULATION_EVENT_FIELD_ON = 0,
    H2_PAL_NFC_CARD_EMULATION_EVENT_ACTIVATED,
    H2_PAL_NFC_CARD_EMULATION_EVENT_CONTENT_ACTIVATED,
    H2_PAL_NFC_CARD_EMULATION_EVENT_CONTENT_ACCESSED,
    H2_PAL_NFC_CARD_EMULATION_EVENT_RAW_FRAME_RECEIVED,
    H2_PAL_NFC_CARD_EMULATION_EVENT_RAW_FRAME_TRANSMITTED,
    H2_PAL_NFC_CARD_EMULATION_EVENT_DEACTIVATED,
    H2_PAL_NFC_CARD_EMULATION_EVENT_FIELD_OFF,
    H2_PAL_NFC_CARD_EMULATION_EVENT_ERROR,
} fm17660k_card_event_type_t;

typedef struct fm17660k_card_event {
    fm17660k_card_event_type_t type;
    h2_pal_result_t result;
    uint32_t content_revision;
    h2_pal_nfc_card_emulation_frame_t frame;
} fm17660k_card_event_t;

typedef struct fm17660k_card_session
    fm17660k_card_session_t;

struct fm17660k_card_session {
    struct h2_fm17660k *owner;
    int open;
    int started;
    int active;
    h2_pal_nfc_card_emulation_mode_t mode;
    uint8_t uid[7];
    h2_nfc_type2_t *type2;
    h2_pal_nfc_card_emulation_raw_exchange_fn raw_exchange;
    void *raw_exchange_user;
    h2_pal_nfc_card_emulation_raw_exchange_fn staged_raw_exchange;
    void *staged_raw_exchange_user;
    uint32_t staged_content_revision;
    int has_staged_raw_content;
    uint32_t content_revision;
    uint8_t frame[FM17660K_CARD_FRAME_MAX_SIZE];
    size_t frame_bit_len;
    uint8_t tx_frame[FM17660K_CARD_FRAME_MAX_SIZE + 2u];
    size_t tx_payload_bit_len;
    size_t tx_frame_bit_len;
    fm17660k_card_poll_phase_t poll_phase;
    uint64_t poll_phase_start_ms;
    uint8_t poll_main_irq;
    uint8_t poll_aux_irq;
    uint8_t poll_page_value;
    uint8_t poll_fifo_control;
    uint8_t poll_frame_length;
    int poll_frame_crc_valid;
    size_t poll_fifo_offset;
    fm17660k_card_event_t events[FM17660K_EVENT_QUEUE_SIZE];
    size_t event_head;
    size_t event_count;
};

struct h2_fm17660k {
    const h2_pal_mem_api_t *mem;
    h2_pal_periph_id_t periph_id;
    h2_fm17660k_transport_t transport;
    uint32_t timeout_ms;
    int initialized;
    fm17660k_frontend_owner_t frontend_owner;
    h2_pal_nfc_api_t reader_api;
    h2_pal_nfc_card_emulation_api_t card_api;
    struct fm17660k_card_session session;
};

static h2_pal_result_t fm17660k_transport_is_valid(
    const h2_fm17660k_transport_t *transport) {
    if (transport == NULL || transport->vtable == NULL ||
        transport->vtable->reset == NULL ||
        transport->vtable->write_reg == NULL ||
        transport->vtable->write_regs == NULL ||
        transport->vtable->read_reg == NULL ||
        transport->vtable->read_regs == NULL ||
        transport->vtable->now_ms == NULL ||
        transport->vtable->sleep_ms == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t fm17660k_write(
    h2_fm17660k_t *fm17660k,
    uint8_t reg,
    uint8_t value) {
    return fm17660k->transport.vtable->write_reg(
        fm17660k->transport.user, reg, value);
}

static h2_pal_result_t fm17660k_read(
    h2_fm17660k_t *fm17660k,
    uint8_t reg,
    uint8_t *out_value) {
    return fm17660k->transport.vtable->read_reg(
        fm17660k->transport.user, reg, out_value);
}

static h2_pal_result_t fm17660k_sleep(
    h2_fm17660k_t *fm17660k,
    uint32_t delay_ms) {
    return fm17660k->transport.vtable->sleep_ms(
        fm17660k->transport.user, delay_ms);
}

static h2_pal_result_t fm17660k_update(
    h2_fm17660k_t *fm17660k,
    uint8_t reg,
    uint8_t clear_mask,
    uint8_t set_mask) {
    uint8_t value;
    h2_pal_result_t rc = fm17660k_read(fm17660k, reg, &value);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    value = (uint8_t)((value & (uint8_t)~clear_mask) | set_mask);
    return fm17660k_write(fm17660k, reg, value);
}

static h2_pal_result_t fm17660k_ext_enter(h2_fm17660k_t *fm17660k) {
    return fm17660k_update(
        fm17660k, H2_FM17660K_REG_PAGE_SELECT, 0u, 0x40u);
}

static h2_pal_result_t fm17660k_ext_leave(h2_fm17660k_t *fm17660k) {
    return fm17660k_update(
        fm17660k, H2_FM17660K_REG_PAGE_SELECT, 0x40u, 0u);
}

static h2_pal_result_t fm17660k_wait_idle(h2_fm17660k_t *fm17660k) {
    uint64_t start_ms = fm17660k->transport.vtable->now_ms(
        fm17660k->transport.user);
    for (size_t index = 0u; index < FM17660K_IDLE_POLL_LIMIT; ++index) {
        uint8_t command;
        h2_pal_result_t rc = fm17660k_read(
            fm17660k, H2_FM17660K_REG_COMMAND, &command);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        if ((command & 0x1fu) == H2_FM17660K_COMMAND_IDLE) {
            return H2_PAL_OK;
        }
        uint64_t now_ms = fm17660k->transport.vtable->now_ms(
            fm17660k->transport.user);
        if (now_ms - start_ms >= fm17660k->timeout_ms) {
            return H2_PAL_ERR_TIMEOUT;
        }
        rc = fm17660k_sleep(fm17660k, 1u);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    return H2_PAL_ERR_TIMEOUT;
}

static h2_pal_result_t fm17660k_wait_power_ready(
    h2_fm17660k_t *fm17660k) {
    uint64_t start_ms = fm17660k->transport.vtable->now_ms(
        fm17660k->transport.user);
    for (size_t index = 0u; index < FM17660K_IDLE_POLL_LIMIT; ++index) {
        uint8_t command;
        h2_pal_result_t rc = fm17660k_read(
            fm17660k, H2_FM17660K_REG_COMMAND, &command);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        if (command == H2_FM17660K_COMMAND_POWER_UP ||
            (command & 0x1fu) == H2_FM17660K_COMMAND_IDLE) {
            return H2_PAL_OK;
        }
        uint64_t now_ms = fm17660k->transport.vtable->now_ms(
            fm17660k->transport.user);
        if (now_ms - start_ms >= fm17660k->timeout_ms) {
            return H2_PAL_ERR_TIMEOUT;
        }
        rc = fm17660k_sleep(fm17660k, 1u);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    return H2_PAL_ERR_TIMEOUT;
}

static h2_pal_result_t fm17660k_fifo_flush(h2_fm17660k_t *fm17660k) {
    return fm17660k_update(
        fm17660k, H2_FM17660K_REG_FIFO_CONTROL, 0u, 0x10u);
}

static h2_pal_result_t fm17660k_reader_fifo_flush(
    h2_fm17660k_t *fm17660k) {
    h2_pal_result_t rc = fm17660k_fifo_flush(fm17660k);
    return rc == H2_PAL_OK ?
        fm17660k_sleep(fm17660k, FM17660K_FIFO_FLUSH_SETTLE_MS) : rc;
}

static h2_pal_result_t fm17660k_reader_set_crc(
    h2_fm17660k_t *fm17660k,
    int enabled) {
    h2_pal_result_t rc = fm17660k_update(
        fm17660k,
        H2_FM17660K_REG_TX_CRC_CONTROL,
        enabled ? 0u : 0x01u,
        enabled ? 0x01u : 0u);
    if (rc == H2_PAL_OK) {
        rc = fm17660k_update(
            fm17660k,
            H2_FM17660K_REG_RX_CRC_CONTROL,
            enabled ? 0u : 0x01u,
            enabled ? 0x01u : 0u);
    }
    return rc;
}

static uint16_t fm17660k_crc_a(const uint8_t *data, size_t data_len) {
    uint16_t crc = 0x6363u;
    for (size_t index = 0u; index < data_len; ++index) {
        crc ^= data[index];
        for (uint8_t bit = 0u; bit < 8u; ++bit) {
            crc = (crc & 1u) != 0u ?
                (uint16_t)((crc >> 1u) ^ 0x8408u) : (uint16_t)(crc >> 1u);
        }
    }
    return crc;
}

static h2_pal_result_t fm17660k_fifo_write_all(
    h2_fm17660k_t *fm17660k,
    const uint8_t *data,
    size_t data_len) {
    return fm17660k->transport.vtable->write_regs(
        fm17660k->transport.user,
        H2_FM17660K_REG_FIFO_DATA,
        data,
        data_len);
}

static h2_pal_result_t fm17660k_fifo_read_exact(
    h2_fm17660k_t *fm17660k,
    uint8_t *data,
    size_t data_len) {
    return fm17660k->transport.vtable->read_regs(
        fm17660k->transport.user,
        H2_FM17660K_REG_FIFO_DATA,
        data,
        data_len);
}

static h2_pal_result_t fm17660k_validate_controller(
    h2_fm17660k_t *fm17660k) {
    h2_pal_result_t rc = fm17660k->transport.vtable->reset(
        fm17660k->transport.user);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = fm17660k_wait_power_ready(fm17660k);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = fm17660k_wait_idle(fm17660k);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = fm17660k_write(
        fm17660k,
        H2_FM17660K_REG_COMMAND,
        H2_FM17660K_COMMAND_SOFT_RESET);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = fm17660k_sleep(fm17660k, 2u);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = fm17660k_wait_idle(fm17660k);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    uint8_t version;
    return fm17660k_read(fm17660k, H2_FM17660K_REG_VERSION, &version);
}

static h2_pal_result_t fm17660k_prepare_reader(h2_fm17660k_t *fm17660k) {
    const uint8_t protocol[] = {0x00u, 0x00u};
    h2_pal_result_t rc = fm17660k_reader_fifo_flush(fm17660k);
    if (rc == H2_PAL_OK) {
        rc = fm17660k_fifo_write_all(fm17660k, protocol, sizeof(protocol));
    }
    if (rc == H2_PAL_OK) {
        rc = fm17660k_write(
            fm17660k, H2_FM17660K_REG_COMMAND,
            H2_FM17660K_COMMAND_LOAD_PROTOCOL);
    }
    if (rc == H2_PAL_OK) {
        rc = fm17660k_wait_idle(fm17660k);
    }
    if (rc == H2_PAL_OK) {
        rc = fm17660k_update(
            fm17660k, H2_FM17660K_REG_TX_MODE, 0x0fu, 0x07u);
    }
    const struct {
        uint8_t reg;
        uint8_t value;
    } setup[] = {
        {H2_FM17660K_REG_TX_AMPLITUDE, 0xffu},
        {H2_FM17660K_REG_TX_CONTROL, 0x40u},
        {H2_FM17660K_REG_FRAME_CONTROL, 0xcfu},
        {H2_FM17660K_REG_RX_CONTROL, 0x04u},
        {H2_FM17660K_REG_RX_WAIT, 0x90u},
        {H2_FM17660K_REG_RECEIVER, 0x12u},
        {H2_FM17660K_REG_RX_ANALOG, 0x09u},
        {H2_FM17660K_REG_WATER_LEVEL, 0x20u},
    };
    for (size_t index = 0u; rc == H2_PAL_OK &&
         index < sizeof(setup) / sizeof(setup[0]); ++index) {
        rc = fm17660k_write(fm17660k, setup[index].reg, setup[index].value);
    }
    return rc;
}

static h2_pal_result_t fm17660k_reader_exchange(
    h2_fm17660k_t *fm17660k,
    const uint8_t *tx,
    size_t tx_len,
    uint8_t tx_last_bits,
    uint8_t *rx,
    size_t rx_capacity,
    size_t *out_rx_len,
    h2_pal_result_t response_timeout_result) {
    *out_rx_len = 0u;
    h2_pal_result_t rc = fm17660k_write(
        fm17660k, H2_FM17660K_REG_COMMAND, H2_FM17660K_COMMAND_IDLE);
    if (rc == H2_PAL_OK) {
        rc = fm17660k_write(fm17660k, H2_FM17660K_REG_IRQ0, 0x7fu);
    }
    if (rc == H2_PAL_OK) {
        rc = fm17660k_reader_fifo_flush(fm17660k);
    }
    if (rc == H2_PAL_OK) {
        rc = fm17660k_write(
            fm17660k, H2_FM17660K_REG_TX_DATA_NUMBER,
            (uint8_t)(0x08u | (tx_last_bits & 7u)));
    }
    if (rc == H2_PAL_OK) {
        rc = fm17660k_fifo_write_all(fm17660k, tx, tx_len);
    }
    if (rc == H2_PAL_OK) {
        rc = fm17660k_write(
            fm17660k, H2_FM17660K_REG_COMMAND,
            H2_FM17660K_COMMAND_TRANSCEIVE);
    }
    if (rc != H2_PAL_OK) {
        return rc;
    }
    uint64_t start_ms = fm17660k->transport.vtable->now_ms(
        fm17660k->transport.user);
    for (size_t poll = 0u; poll < FM17660K_IDLE_POLL_LIMIT; ++poll) {
        uint64_t now_ms = fm17660k->transport.vtable->now_ms(
            fm17660k->transport.user);
        uint64_t elapsed_ms = now_ms - start_ms;
        if (elapsed_ms < FM17660K_READER_RX_GRACE_MS) {
            if (elapsed_ms >= fm17660k->timeout_ms) {
                return response_timeout_result;
            }
            rc = fm17660k_sleep(fm17660k, 1u);
            if (rc != H2_PAL_OK) {
                return rc;
            }
            continue;
        }
        uint8_t irq;
        rc = fm17660k_read(fm17660k, H2_FM17660K_REG_IRQ0, &irq);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        if ((irq & 0x02u) != 0u) {
            uint8_t error;
            rc = fm17660k_read(fm17660k, H2_FM17660K_REG_ERROR, &error);
            if (rc != H2_PAL_OK) {
                return rc;
            }
            return (error & 0x08u) != 0u ? response_timeout_result
                                         : H2_PAL_ERR_IO;
        }
        if ((irq & 0x04u) != 0u) {
            uint8_t error;
            uint8_t length;
            rc = fm17660k_read(fm17660k, H2_FM17660K_REG_ERROR, &error);
            if (rc != H2_PAL_OK || error != 0u) {
                return rc != H2_PAL_OK ? rc : H2_PAL_ERR_IO;
            }
            rc = fm17660k_read(
                fm17660k, H2_FM17660K_REG_FIFO_LENGTH, &length);
            if (rc != H2_PAL_OK) {
                return rc;
            }
            if ((size_t)length > rx_capacity) {
                (void)fm17660k_fifo_flush(fm17660k);
                return H2_PAL_ERR_TRUNCATED;
            }
            rc = fm17660k_fifo_read_exact(fm17660k, rx, length);
            if (rc == H2_PAL_OK) {
                *out_rx_len = length;
            }
            return rc;
        }
        if ((irq & 0x10u) != 0u || elapsed_ms >= fm17660k->timeout_ms) {
            return response_timeout_result;
        }
        rc = fm17660k_sleep(fm17660k, 1u);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    return response_timeout_result;
}

static h2_pal_result_t fm17660k_reader_select_level(
    h2_fm17660k_t *fm17660k,
    uint8_t select_command,
    const uint8_t cascade[5],
    uint8_t *out_sak) {
    const uint8_t select[] = {
        select_command,
        0x70u,
        cascade[0],
        cascade[1],
        cascade[2],
        cascade[3],
        cascade[4],
    };
    uint8_t response[3];
    size_t response_len = 0u;
    h2_pal_result_t rc = fm17660k_reader_set_crc(fm17660k, 1);
    if (rc == H2_PAL_OK) {
        rc = fm17660k_reader_exchange(
            fm17660k,
            select,
            sizeof(select),
            0u,
            response,
            sizeof(response),
            &response_len,
            H2_PAL_ERR_TIMEOUT);
    }
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (response_len == 0u) {
        return H2_PAL_ERR_FORMAT;
    }
    *out_sak = response[0];
    return H2_PAL_OK;
}

static h2_pal_result_t fm17660k_reader_anticollision_level(
    h2_fm17660k_t *fm17660k,
    uint8_t select_command,
    uint8_t out_cascade[5],
    uint8_t *out_sak) {
    const uint8_t anticollision[] = {select_command, 0x20u};
    size_t response_len = 0u;
    h2_pal_result_t rc = fm17660k_reader_set_crc(fm17660k, 0);
    if (rc == H2_PAL_OK) {
        rc = fm17660k_reader_exchange(
            fm17660k,
            anticollision,
            sizeof(anticollision),
            0u,
            out_cascade,
            5u,
            &response_len,
            H2_PAL_ERR_TIMEOUT);
    }
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (response_len != 5u ||
        out_cascade[4] !=
            (uint8_t)(out_cascade[0] ^ out_cascade[1] ^ out_cascade[2] ^
                      out_cascade[3])) {
        return H2_PAL_ERR_FORMAT;
    }
    return fm17660k_reader_select_level(
        fm17660k, select_command, out_cascade, out_sak);
}

static h2_pal_result_t fm17660k_reader_scan(
    void *user,
    h2_pal_periph_id_t periph_id,
    h2_pal_nfc_scan_t *out_scan) {
    h2_fm17660k_t *fm17660k = user;
    memset(out_scan, 0, sizeof(*out_scan));
    out_scan->id = periph_id;
    if (fm17660k == NULL || !fm17660k->initialized) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (periph_id != fm17660k->periph_id) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    if (fm17660k->frontend_owner == FM17660K_FRONTEND_CARD_EMULATION) {
        return H2_PAL_ERR_BUSY;
    }
    fm17660k->frontend_owner = FM17660K_FRONTEND_READER;
    h2_pal_result_t rc = fm17660k_prepare_reader(fm17660k);
    uint8_t atqa[2];
    size_t atqa_len = 0u;
    const uint8_t reqa = 0x26u;
    if (rc == H2_PAL_OK) {
        rc = fm17660k_reader_set_crc(fm17660k, 0);
    }
    if (rc == H2_PAL_OK) {
        rc = fm17660k_reader_exchange(
            fm17660k,
            &reqa,
            1u,
            7u,
            atqa,
            sizeof(atqa),
            &atqa_len,
            H2_PAL_ERR_NOT_FOUND);
    }
    if (rc == H2_PAL_ERR_NOT_FOUND) {
        fm17660k->frontend_owner = FM17660K_FRONTEND_NONE;
        out_scan->stage = H2_PAL_NFC_STAGE_ABSENT;
        out_scan->result = H2_PAL_OK;
        return H2_PAL_OK;
    }
    if (rc == H2_PAL_OK && atqa_len != sizeof(atqa)) {
        rc = H2_PAL_ERR_FORMAT;
    }
    size_t expected_uid_len = 0u;
    if (rc == H2_PAL_OK) {
        switch (atqa[0] & 0xc0u) {
        case 0x00u:
            expected_uid_len = 4u;
            break;
        case 0x40u:
            expected_uid_len = 7u;
            break;
        case 0x80u:
            expected_uid_len = 10u;
            break;
        default:
            rc = H2_PAL_ERR_FORMAT;
            break;
        }
    }
    const uint8_t select_commands[] = {0x93u, 0x95u, 0x97u};
    for (size_t level = 0u;
         rc == H2_PAL_OK && level < sizeof(select_commands);
         ++level) {
        uint8_t cascade[5];
        uint8_t sak = 0u;
        rc = fm17660k_reader_anticollision_level(
            fm17660k, select_commands[level], cascade, &sak);
        if (rc != H2_PAL_OK) {
            break;
        }
        int cascade_continues = cascade[0] == 0x88u;
        if (cascade_continues != ((sak & 0x04u) != 0u)) {
            rc = H2_PAL_ERR_FORMAT;
            break;
        }
        if (cascade_continues) {
            if (level + 1u == sizeof(select_commands)) {
                rc = H2_PAL_ERR_FORMAT;
                break;
            }
            memcpy(&out_scan->uid[out_scan->uid_len], &cascade[1], 3u);
            out_scan->uid_len += 3u;
        } else {
            memcpy(&out_scan->uid[out_scan->uid_len], cascade, 4u);
            out_scan->uid_len += 4u;
            break;
        }
    }
    if (rc == H2_PAL_OK && out_scan->uid_len != expected_uid_len) {
        rc = H2_PAL_ERR_FORMAT;
    }
    fm17660k->frontend_owner = FM17660K_FRONTEND_NONE;
    if (rc == H2_PAL_OK && out_scan->uid_len != 0u) {
        out_scan->stage = H2_PAL_NFC_STAGE_DISCOVERED;
        out_scan->result = H2_PAL_OK;
        out_scan->tag_type = H2_PAL_NFC_TAG_TYPE_ISO14443A;
        return H2_PAL_OK;
    }
    out_scan->uid_len = 0u;
    memset(out_scan->uid, 0, sizeof(out_scan->uid));
    out_scan->stage = H2_PAL_NFC_STAGE_ERROR;
    if (rc == H2_PAL_OK) {
        rc = H2_PAL_ERR_FORMAT;
    }
    out_scan->result = rc;
    return rc;
}

static h2_pal_result_t fm17660k_reader_read(
    void *user,
    h2_pal_periph_id_t periph_id,
    const uint8_t *expected_uid,
    uint8_t expected_uid_len,
    h2_pal_nfc_data_type_t requested_type,
    const h2_pal_mem_api_t *allocator,
    h2_pal_nfc_data_read_t *out_data) {
    h2_fm17660k_t *fm17660k = user;
    memset(out_data, 0, sizeof(*out_data));
    if (fm17660k == NULL || !fm17660k->initialized) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (periph_id != fm17660k->periph_id) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    if (fm17660k->frontend_owner == FM17660K_FRONTEND_CARD_EMULATION) {
        return H2_PAL_ERR_BUSY;
    }
    if (requested_type != H2_PAL_NFC_DATA_RAW &&
        requested_type != H2_PAL_NFC_DATA_NTAG_PAGES &&
        requested_type != H2_PAL_NFC_DATA_NDEF) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    h2_pal_nfc_scan_t scan;
    h2_pal_result_t rc = fm17660k_reader_scan(user, periph_id, &scan);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (scan.stage != H2_PAL_NFC_STAGE_DISCOVERED) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    if (expected_uid_len != 0u &&
        (scan.uid_len != expected_uid_len ||
         memcmp(scan.uid, expected_uid, expected_uid_len) != 0)) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    fm17660k->frontend_owner = FM17660K_FRONTEND_READER;
    const uint8_t read_page4[] = {0x30u, 0x04u};
    uint8_t response[18];
    size_t response_len = 0u;
    rc = fm17660k_reader_set_crc(fm17660k, 1);
    if (rc == H2_PAL_OK) {
        rc = fm17660k_reader_exchange(
            fm17660k, read_page4, sizeof(read_page4), 0u,
            response, sizeof(response), &response_len,
            H2_PAL_ERR_TIMEOUT);
    }
    fm17660k->frontend_owner = FM17660K_FRONTEND_NONE;
    if (rc != H2_PAL_OK || response_len < 16u) {
        return rc != H2_PAL_OK ? rc : H2_PAL_ERR_FORMAT;
    }
    size_t offset = 0u;
    size_t length = 16u;
    if (requested_type == H2_PAL_NFC_DATA_NDEF) {
        if (response[0] != 0x03u || response[1] > 14u) {
            return H2_PAL_ERR_FORMAT;
        }
        offset = 2u;
        length = response[1];
    }
    uint8_t *copy = NULL;
    if (length != 0u) {
        copy = h2_pal_mem_alloc(allocator, length);
        if (copy == NULL) {
            return H2_PAL_ERR_NO_MEMORY;
        }
        memcpy(copy, &response[offset], length);
    }
    *out_data = (h2_pal_nfc_data_read_t){
        .id = periph_id,
        .tag_type = H2_PAL_NFC_TAG_TYPE_ISO14443A,
        .uid_len = scan.uid_len,
        .type = requested_type,
        .bytes = copy,
        .len = length,
    };
    memcpy(out_data->uid, scan.uid, scan.uid_len);
    return H2_PAL_OK;
}

static h2_pal_result_t fm17660k_event_push(
    struct fm17660k_card_session *session,
    fm17660k_card_event_type_t type,
    h2_pal_result_t result,
    const uint8_t *frame,
    size_t frame_bit_len) {
    if (session->event_count == FM17660K_EVENT_QUEUE_SIZE) {
        return H2_PAL_ERR_FULL;
    }
    size_t index =
        (session->event_head + session->event_count) % FM17660K_EVENT_QUEUE_SIZE;
    session->events[index] = (fm17660k_card_event_t){
        .type = type,
        .result = result,
        .content_revision = session->content_revision,
        .frame = {
            .bytes = frame,
            .bit_len = frame_bit_len,
        },
    };
    session->event_count++;
    return H2_PAL_OK;
}

static h2_pal_result_t fm17660k_event_pop(
    struct fm17660k_card_session *session,
    fm17660k_card_event_t *out_event) {
    if (session->event_count == 0u) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    *out_event = session->events[session->event_head];
    session->event_head = (session->event_head + 1u) % FM17660K_EVENT_QUEUE_SIZE;
    session->event_count--;
    return H2_PAL_OK;
}

static h2_pal_result_t fm17660k_card_get_capabilities(
    void *user,
    h2_pal_periph_id_t periph_id,
    h2_pal_nfc_card_emulation_capabilities_t *out_capabilities) {
    h2_fm17660k_t *fm17660k = user;
    if (fm17660k == NULL || !fm17660k->initialized) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (periph_id != fm17660k->periph_id) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    *out_capabilities = (h2_pal_nfc_card_emulation_capabilities_t){
        .technology_mask =
            H2_PAL_NFC_CARD_EMULATION_TECHNOLOGY_ISO14443A,
        .managed_profile_mask =
            H2_PAL_NFC_CARD_EMULATION_PROFILE_TYPE2_READ_ONLY,
        .exchange_mode_mask =
            H2_PAL_NFC_CARD_EMULATION_MODE_MANAGED_PROFILE |
            H2_PAL_NFC_CARD_EMULATION_MODE_RAW_FRAME,
        .min_uid_len = 7u,
        .max_uid_len = 7u,
        .max_managed_content_len = H2_NFC_TYPE2_NDEF_MAX_SIZE,
        .raw = {
            .max_rx_frame_size = FM17660K_CARD_FRAME_MAX_SIZE,
            .max_tx_frame_size = FM17660K_CARD_FRAME_MAX_SIZE,
            .response_deadline_us =
                FM17660K_RAW_RESPONSE_DEADLINE_MS * 1000u,
            .supports_partial_bytes = 0,
            .provider_owns_crc = 1,
            .provider_owns_parity = 1,
            .provider_owns_activation = 1,
        },
    };
    return H2_PAL_OK;
}

static h2_pal_result_t fm17660k_card_open(
    void *user,
    const fm17660k_card_open_config_t *config,
    fm17660k_card_session_t **out_session) {
    h2_fm17660k_t *fm17660k = user;
    if (fm17660k == NULL || !fm17660k->initialized) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (config->periph_id != fm17660k->periph_id) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    if (config->technology !=
            H2_PAL_NFC_CARD_EMULATION_TECHNOLOGY_ISO14443A ||
        config->uid_len != 7u ||
        (config->mode != H2_PAL_NFC_CARD_EMULATION_MODE_MANAGED_PROFILE &&
         config->mode != H2_PAL_NFC_CARD_EMULATION_MODE_RAW_FRAME) ||
        (config->mode == H2_PAL_NFC_CARD_EMULATION_MODE_MANAGED_PROFILE &&
         config->managed_profile !=
             H2_PAL_NFC_CARD_EMULATION_PROFILE_TYPE2_READ_ONLY) ||
        (config->mode == H2_PAL_NFC_CARD_EMULATION_MODE_RAW_FRAME &&
         config->managed_profile != H2_PAL_NFC_CARD_EMULATION_PROFILE_NONE)) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (fm17660k->frontend_owner != FM17660K_FRONTEND_NONE ||
        fm17660k->session.open) {
        return H2_PAL_ERR_BUSY;
    }
    memset(&fm17660k->session, 0, sizeof(fm17660k->session));
    fm17660k->session.owner = fm17660k;
    fm17660k->session.open = 1;
    fm17660k->session.mode = config->mode;
    memcpy(fm17660k->session.uid, config->uid, config->uid_len);
    if (config->mode == H2_PAL_NFC_CARD_EMULATION_MODE_MANAGED_PROFILE) {
        const h2_nfc_type2_config_t type2_config = {
            .mem = fm17660k->mem,
            .uid = config->uid,
            .uid_len = config->uid_len,
            .enable_fast_read = 1,
        };
        h2_pal_result_t rc = h2_nfc_type2_create(
            &type2_config, &fm17660k->session.type2);
        if (rc != H2_PAL_OK) {
            memset(&fm17660k->session, 0, sizeof(fm17660k->session));
            return rc;
        }
    }
    fm17660k->frontend_owner = FM17660K_FRONTEND_CARD_EMULATION;
    *out_session = &fm17660k->session;
    return H2_PAL_OK;
}

static h2_pal_result_t fm17660k_card_set_content(
    void *user,
    fm17660k_card_session_t *session,
    const fm17660k_card_content_t *content) {
    h2_fm17660k_t *fm17660k = user;
    if (fm17660k == NULL || session != &fm17660k->session || !session->open) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (content->mode != session->mode) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (session->mode == H2_PAL_NFC_CARD_EMULATION_MODE_MANAGED_PROFILE) {
        return h2_nfc_type2_set_ndef(
            session->type2,
            content->managed_bytes,
            content->managed_len,
            content->revision);
    }
    if (session->active) {
        session->staged_raw_exchange = content->raw_exchange;
        session->staged_raw_exchange_user = content->raw_exchange_user;
        session->staged_content_revision = content->revision;
        session->has_staged_raw_content = 1;
    } else {
        session->raw_exchange = content->raw_exchange;
        session->raw_exchange_user = content->raw_exchange_user;
        session->content_revision = content->revision;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t fm17660k_card_configure(
    h2_fm17660k_t *fm17660k,
    struct fm17660k_card_session *session) {
    h2_pal_result_t rc = fm17660k_write(
        fm17660k, H2_FM17660K_REG_COMMAND, H2_FM17660K_COMMAND_IDLE);
    if (rc != H2_PAL_OK || (rc = fm17660k_ext_enter(fm17660k)) != H2_PAL_OK) {
        return rc;
    }
    const struct {
        uint8_t reg;
        uint8_t value;
    } setup[] = {
        {H2_FM17660K_REG_CARD_CONTROL, 0x01u},
    };
    for (size_t index = 0u; rc == H2_PAL_OK &&
         index < sizeof(setup) / sizeof(setup[0]); ++index) {
        rc = fm17660k_write(fm17660k, setup[index].reg, setup[index].value);
    }
    if (rc == H2_PAL_OK) {
        rc = fm17660k_sleep(fm17660k, 2u);
        if (rc == H2_PAL_OK) {
            rc = fm17660k_write(
                fm17660k, H2_FM17660K_REG_CARD_CONTROL, 0x00u);
        }
    }
    uint8_t rf_detect_config = 0u;
    if (rc == H2_PAL_OK) {
        rc = fm17660k_read(
            fm17660k,
            H2_FM17660K_REG_CARD_RF_DETECT_CONFIG,
            &rf_detect_config);
    }
    if (rc == H2_PAL_OK) {
        rc = fm17660k_write(
            fm17660k,
            H2_FM17660K_REG_CARD_RF_DETECT_CONFIG,
            (uint8_t)(rf_detect_config & 0xf0u));
    }
    const struct {
        uint8_t reg;
        uint8_t value;
    } profile_header[] = {
        {H2_FM17660K_REG_TAG_CONTROL, 0xa2u},
        {H2_FM17660K_REG_CARD_DATA_CONFIG, 0x00u},
        {H2_FM17660K_REG_CARD_ATQA0, 0x44u},
        {H2_FM17660K_REG_CARD_ATQA1, 0x00u},
    };
    for (size_t index = 0u; rc == H2_PAL_OK &&
         index < sizeof(profile_header) / sizeof(profile_header[0]); ++index) {
        rc = fm17660k_write(
            fm17660k,
            profile_header[index].reg,
            profile_header[index].value);
    }
    for (size_t index = 0u; rc == H2_PAL_OK && index < 7u; ++index) {
        rc = fm17660k_write(
            fm17660k,
            (uint8_t)(H2_FM17660K_REG_CARD_UID0 + index),
            session->uid[index]);
    }
    const uint8_t ats[] = {0x05u, 0x75u, 0x00u, 0x81u, 0x00u};
    for (size_t index = 0u; rc == H2_PAL_OK && index < sizeof(ats); ++index) {
        rc = fm17660k_write(
            fm17660k,
            (uint8_t)(H2_FM17660K_REG_CARD_ATS0 + index),
            ats[index]);
    }
    const struct {
        uint8_t reg;
        uint8_t value;
    } profile_enable[] = {
        {H2_FM17660K_REG_CARD_SAK1, 0x04u},
        {H2_FM17660K_REG_CARD_SAK2, 0x00u},
        {H2_FM17660K_REG_CARD_TX_CONFIG, 0xf0u},
        {H2_FM17660K_REG_CARD_MAIN_IRQ, 0x00u},
        {H2_FM17660K_REG_CARD_AUX_IRQ, 0x00u},
        {H2_FM17660K_REG_CARD_MAIN_IRQ_ENABLE, 0xffu},
        {H2_FM17660K_REG_CARD_AUX_IRQ_ENABLE, 0x0fu},
        {H2_FM17660K_REG_NC_MODE, 0x80u},
        {H2_FM17660K_REG_RF_COMMAND, H2_FM17660K_RF_COMMAND_RX},
    };
    for (size_t index = 0u; rc == H2_PAL_OK &&
         index < sizeof(profile_enable) / sizeof(profile_enable[0]); ++index) {
        rc = fm17660k_write(
            fm17660k,
            profile_enable[index].reg,
            profile_enable[index].value);
    }
    h2_pal_result_t leave_rc = fm17660k_ext_leave(fm17660k);
    if (rc == H2_PAL_OK) {
        rc = leave_rc;
    }
    if (rc == H2_PAL_OK) {
        rc = fm17660k_update(
            fm17660k, H2_FM17660K_REG_TX_MODE, 0x0bu, 0u);
    }
    if (rc == H2_PAL_OK) {
        rc = fm17660k_update(
            fm17660k, H2_FM17660K_REG_RX_TX_CONTROL, 0x0fu, 0u);
    }
    return rc;
}

static h2_pal_result_t fm17660k_card_start(
    void *user,
    fm17660k_card_session_t *session) {
    h2_fm17660k_t *fm17660k = user;
    if (fm17660k == NULL || session != &fm17660k->session || !session->open) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (session->started) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (fm17660k->frontend_owner != FM17660K_FRONTEND_CARD_EMULATION) {
        return H2_PAL_ERR_BUSY;
    }
    if (session->mode == H2_PAL_NFC_CARD_EMULATION_MODE_RAW_FRAME &&
        session->raw_exchange == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    h2_pal_result_t rc = fm17660k_card_configure(fm17660k, session);
    if (rc == H2_PAL_OK) {
        session->started = 1;
    }
    return rc;
}

static h2_pal_result_t fm17660k_card_prepare_tx(
    struct fm17660k_card_session *session,
    const uint8_t *payload,
    size_t payload_bit_len) {
    if (payload == NULL ||
        payload_bit_len > FM17660K_CARD_FRAME_MAX_SIZE * 8u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if ((payload_bit_len & 7u) != 0u) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    size_t payload_len = payload_bit_len / 8u;
    const size_t crc_len = 2u;
    if (payload_len + crc_len > sizeof(session->tx_frame)) {
        return H2_PAL_ERR_TRUNCATED;
    }
    if (payload != session->tx_frame && payload_len != 0u) {
        memcpy(session->tx_frame, payload, payload_len);
    }
    if (crc_len != 0u) {
        uint16_t crc = fm17660k_crc_a(payload, payload_len);
        session->tx_frame[payload_len] = (uint8_t)crc;
        session->tx_frame[payload_len + 1u] = (uint8_t)(crc >> 8u);
    }
    session->tx_payload_bit_len = payload_bit_len;
    session->tx_frame_bit_len = payload_bit_len + crc_len * 8u;
    return H2_PAL_OK;
}

static h2_pal_result_t fm17660k_card_prepare_response(
    h2_fm17660k_t *fm17660k,
    struct fm17660k_card_session *session) {
    if ((session->frame_bit_len & 7u) != 0u) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    size_t frame_len = (session->frame_bit_len + 7u) / 8u;
    if (session->mode == H2_PAL_NFC_CARD_EMULATION_MODE_RAW_FRAME) {
        size_t response_bit_len = 0u;
        const h2_pal_nfc_card_emulation_frame_t frame = {
            .bytes = session->frame,
            .bit_len = session->frame_bit_len,
        };
        uint64_t callback_start_ms = fm17660k->transport.vtable->now_ms(
            fm17660k->transport.user);
        h2_pal_result_t rc = session->raw_exchange(
            session->raw_exchange_user,
            &frame,
            session->tx_frame,
            FM17660K_CARD_FRAME_MAX_SIZE,
            &response_bit_len);
        uint64_t callback_elapsed_ms =
            fm17660k->transport.vtable->now_ms(
                fm17660k->transport.user) - callback_start_ms;
        if (callback_elapsed_ms > FM17660K_RAW_RESPONSE_DEADLINE_MS) {
            rc = H2_PAL_ERR_TIMEOUT;
        }
        if (rc == H2_PAL_OK &&
            response_bit_len > FM17660K_CARD_FRAME_MAX_SIZE * 8u) {
            rc = H2_PAL_ERR_TRUNCATED;
        }
        (void)fm17660k_event_push(
            session,
            H2_PAL_NFC_CARD_EMULATION_EVENT_RAW_FRAME_RECEIVED,
            H2_PAL_OK,
            session->frame,
            session->frame_bit_len);
        if (rc == H2_PAL_OK && response_bit_len != 0u) {
            return fm17660k_card_prepare_tx(
                session, session->tx_frame, response_bit_len);
        }
        if (rc == H2_PAL_ERR_WOULD_BLOCK) {
            session->tx_frame_bit_len = 0u;
            return H2_PAL_OK;
        }
        session->tx_frame_bit_len = 0u;
        return rc;
    }
    size_t response_len = 0u;
    h2_pal_result_t rc = h2_nfc_type2_process(
        session->type2,
        session->frame,
        frame_len,
        session->tx_frame,
        FM17660K_CARD_FRAME_MAX_SIZE,
        &response_len);
    if (rc == H2_PAL_OK && response_len != 0u) {
        rc = fm17660k_card_prepare_tx(
            session, session->tx_frame, response_len * 8u);
    } else if (rc == H2_PAL_OK) {
        session->tx_frame_bit_len = 0u;
    }
    if (rc == H2_PAL_OK && frame_len != 0u &&
        (session->frame[0] == 0x30u || session->frame[0] == 0x3au)) {
        (void)fm17660k_event_push(
            session,
            H2_PAL_NFC_CARD_EMULATION_EVENT_CONTENT_ACCESSED,
            H2_PAL_OK,
            NULL,
            0u);
    }
    return rc;
}

static void fm17660k_card_poll_set_phase(
    h2_fm17660k_t *fm17660k,
    struct fm17660k_card_session *session,
    fm17660k_card_poll_phase_t phase) {
    session->poll_phase = phase;
    session->poll_phase_start_ms = fm17660k->transport.vtable->now_ms(
        fm17660k->transport.user);
}

static h2_pal_result_t fm17660k_card_poll_pending_result(
    h2_fm17660k_t *fm17660k,
    const struct fm17660k_card_session *session) {
    uint64_t elapsed_ms = fm17660k->transport.vtable->now_ms(
        fm17660k->transport.user) - session->poll_phase_start_ms;
    return elapsed_ms >= fm17660k->timeout_ms ?
        H2_PAL_ERR_TIMEOUT : H2_PAL_ERR_WOULD_BLOCK;
}

static int fm17660k_card_finish_receive(
    struct fm17660k_card_session *session,
    uint8_t last_bits) {
    size_t length = session->poll_frame_length;
    session->frame_bit_len = (last_bits & 7u) == 0u ?
        length * 8u : (length - 1u) * 8u + (last_bits & 7u);
    if ((session->frame_bit_len & 7u) != 0u) {
        uint8_t used_last_bits = (uint8_t)(session->frame_bit_len & 7u);
        session->frame[length - 1u] &=
            (uint8_t)(0xffu << (8u - used_last_bits));
    }
    if ((session->frame_bit_len & 7u) == 0u &&
        session->frame_bit_len >= 24u) {
        size_t frame_len = session->frame_bit_len / 8u;
        uint16_t expected = fm17660k_crc_a(session->frame, frame_len - 2u);
        uint16_t received = (uint16_t)session->frame[frame_len - 2u] |
            (uint16_t)((uint16_t)session->frame[frame_len - 1u] << 8u);
        if (expected == received) {
            session->frame_bit_len -= 16u;
            return 1;
        }
    }
    return 0;
}

static h2_pal_result_t fm17660k_card_poll_step(
    h2_fm17660k_t *fm17660k,
    struct fm17660k_card_session *session) {
    h2_pal_result_t rc = H2_PAL_OK;
    uint8_t value = 0u;

    switch (session->poll_phase) {
    case FM17660K_CARD_POLL_IRQ_ENTER_READ:
        rc = fm17660k_read(
            fm17660k, H2_FM17660K_REG_PAGE_SELECT,
            &session->poll_page_value);
        if (rc == H2_PAL_OK) {
            fm17660k_card_poll_set_phase(
                fm17660k, session, FM17660K_CARD_POLL_IRQ_ENTER_WRITE);
        }
        break;
    case FM17660K_CARD_POLL_IRQ_ENTER_WRITE:
        rc = fm17660k_write(
            fm17660k, H2_FM17660K_REG_PAGE_SELECT,
            (uint8_t)(session->poll_page_value | 0x40u));
        if (rc == H2_PAL_OK) {
            fm17660k_card_poll_set_phase(
                fm17660k, session, FM17660K_CARD_POLL_IRQ_READ_MAIN);
        }
        break;
    case FM17660K_CARD_POLL_IRQ_READ_MAIN:
        rc = fm17660k_read(
            fm17660k, H2_FM17660K_REG_CARD_MAIN_IRQ,
            &session->poll_main_irq);
        if (rc == H2_PAL_OK) {
            fm17660k_card_poll_set_phase(
                fm17660k, session, FM17660K_CARD_POLL_IRQ_READ_AUX);
        }
        break;
    case FM17660K_CARD_POLL_IRQ_READ_AUX:
        rc = fm17660k_read(
            fm17660k, H2_FM17660K_REG_CARD_AUX_IRQ,
            &session->poll_aux_irq);
        if (rc == H2_PAL_OK) {
            fm17660k_card_poll_set_phase(
                fm17660k, session, FM17660K_CARD_POLL_IRQ_ACK_MAIN);
        }
        break;
    case FM17660K_CARD_POLL_IRQ_ACK_MAIN:
        if (session->poll_main_irq != 0u) {
            rc = fm17660k_write(
                fm17660k, H2_FM17660K_REG_CARD_MAIN_IRQ, 0x00u);
        }
        if (rc == H2_PAL_OK) {
            fm17660k_card_poll_set_phase(
                fm17660k, session, FM17660K_CARD_POLL_IRQ_ACK_AUX);
        }
        break;
    case FM17660K_CARD_POLL_IRQ_ACK_AUX:
        if (session->poll_aux_irq != 0u) {
            rc = fm17660k_write(
                fm17660k, H2_FM17660K_REG_CARD_AUX_IRQ, 0x00u);
        }
        if (rc == H2_PAL_OK) {
            fm17660k_card_poll_set_phase(
                fm17660k, session, FM17660K_CARD_POLL_IRQ_LEAVE_READ);
        }
        break;
    case FM17660K_CARD_POLL_IRQ_LEAVE_READ:
        rc = fm17660k_read(
            fm17660k, H2_FM17660K_REG_PAGE_SELECT,
            &session->poll_page_value);
        if (rc == H2_PAL_OK) {
            fm17660k_card_poll_set_phase(
                fm17660k, session, FM17660K_CARD_POLL_IRQ_LEAVE_WRITE);
        }
        break;
    case FM17660K_CARD_POLL_IRQ_LEAVE_WRITE:
        rc = fm17660k_write(
            fm17660k, H2_FM17660K_REG_PAGE_SELECT,
            (uint8_t)(session->poll_page_value & 0xbfu));
        if (rc == H2_PAL_OK) {
            fm17660k_card_poll_set_phase(
                fm17660k, session, FM17660K_CARD_POLL_PROCESS);
        }
        break;
    case FM17660K_CARD_POLL_PROCESS:
        if ((session->poll_main_irq & H2_FM17660K_CARD_IRQ_RF_ON) != 0u) {
            (void)fm17660k_event_push(
                session, H2_PAL_NFC_CARD_EMULATION_EVENT_FIELD_ON,
                H2_PAL_OK, NULL, 0u);
        }
        if ((session->poll_main_irq & H2_FM17660K_CARD_IRQ_ACTIVE) != 0u &&
            !session->active) {
            session->active = 1;
            (void)fm17660k_event_push(
                session, H2_PAL_NFC_CARD_EMULATION_EVENT_ACTIVATED,
                H2_PAL_OK, NULL, 0u);
            if (session->type2 != NULL) {
                rc = h2_nfc_type2_activate(
                    session->type2, &session->content_revision);
            } else {
                if (session->has_staged_raw_content) {
                    session->raw_exchange = session->staged_raw_exchange;
                    session->raw_exchange_user =
                        session->staged_raw_exchange_user;
                    session->content_revision =
                        session->staged_content_revision;
                    session->has_staged_raw_content = 0;
                }
            }
            if (rc == H2_PAL_OK) {
                (void)fm17660k_event_push(
                    session,
                    H2_PAL_NFC_CARD_EMULATION_EVENT_CONTENT_ACTIVATED,
                    H2_PAL_OK, NULL, 0u);
            }
        }
        if (rc == H2_PAL_OK &&
            (session->poll_main_irq & H2_FM17660K_CARD_IRQ_RX_END) != 0u) {
            session->poll_fifo_offset = 0u;
            fm17660k_card_poll_set_phase(
                fm17660k, session, FM17660K_CARD_POLL_RX_LENGTH);
        } else if (rc == H2_PAL_OK) {
            fm17660k_card_poll_set_phase(
                fm17660k, session, FM17660K_CARD_POLL_POSTPROCESS);
        }
        break;
    case FM17660K_CARD_POLL_RX_LENGTH:
        rc = fm17660k_read(
            fm17660k, H2_FM17660K_REG_FIFO_LENGTH,
            &session->poll_frame_length);
        if (rc == H2_PAL_OK && session->poll_frame_length == 0u) {
            rc = fm17660k_card_poll_pending_result(fm17660k, session);
        } else if (rc == H2_PAL_OK) {
            fm17660k_card_poll_set_phase(
                fm17660k, session, FM17660K_CARD_POLL_RX_FIFO);
        }
        break;
    case FM17660K_CARD_POLL_RX_FIFO:
        rc = fm17660k->transport.vtable->read_regs(
            fm17660k->transport.user,
            H2_FM17660K_REG_FIFO_DATA,
            &session->frame[session->poll_fifo_offset],
            (size_t)session->poll_frame_length - session->poll_fifo_offset);
        if (rc == H2_PAL_OK) {
            session->poll_fifo_offset = session->poll_frame_length;
            fm17660k_card_poll_set_phase(
                fm17660k, session, FM17660K_CARD_POLL_RX_LAST_BITS);
        }
        break;
    case FM17660K_CARD_POLL_RX_LAST_BITS:
        rc = fm17660k_read(
            fm17660k, H2_FM17660K_REG_RX_BIT_CONTROL, &value);
        if (rc == H2_PAL_OK) {
            session->poll_frame_crc_valid =
                fm17660k_card_finish_receive(session, value);
            fm17660k_card_poll_set_phase(
                fm17660k, session, FM17660K_CARD_POLL_PREPARE_RESPONSE);
        }
        break;
    case FM17660K_CARD_POLL_PREPARE_RESPONSE:
        session->tx_frame_bit_len = 0u;
        if ((session->frame_bit_len & 7u) == 0u &&
            !session->poll_frame_crc_valid) {
            rc = H2_PAL_ERR_FORMAT;
            break;
        }
        rc = fm17660k_card_prepare_response(fm17660k, session);
        if (rc == H2_PAL_OK && session->tx_frame_bit_len != 0u) {
            session->poll_fifo_offset = 0u;
            fm17660k_card_poll_set_phase(
                fm17660k, session, FM17660K_CARD_POLL_TX_FLUSH_READ);
        } else if (rc == H2_PAL_OK) {
            fm17660k_card_poll_set_phase(
                fm17660k, session, FM17660K_CARD_POLL_POSTPROCESS);
        }
        break;
    case FM17660K_CARD_POLL_TX_FLUSH_READ:
        rc = fm17660k_read(
            fm17660k, H2_FM17660K_REG_FIFO_CONTROL,
            &session->poll_fifo_control);
        if (rc == H2_PAL_OK) {
            fm17660k_card_poll_set_phase(
                fm17660k, session, FM17660K_CARD_POLL_TX_FLUSH_WRITE);
        }
        break;
    case FM17660K_CARD_POLL_TX_FLUSH_WRITE:
        rc = fm17660k_write(
            fm17660k, H2_FM17660K_REG_FIFO_CONTROL,
            (uint8_t)(session->poll_fifo_control | 0x10u));
        if (rc == H2_PAL_OK) {
            fm17660k_card_poll_set_phase(
                fm17660k, session, FM17660K_CARD_POLL_TX_FIFO);
        }
        break;
    case FM17660K_CARD_POLL_TX_FIFO: {
        size_t frame_len = (session->tx_frame_bit_len + 7u) / 8u;
        rc = fm17660k->transport.vtable->write_regs(
            fm17660k->transport.user,
            H2_FM17660K_REG_FIFO_DATA,
            &session->tx_frame[session->poll_fifo_offset],
            frame_len - session->poll_fifo_offset);
        if (rc == H2_PAL_OK) {
            session->poll_fifo_offset = frame_len;
            fm17660k_card_poll_set_phase(
                fm17660k, session, FM17660K_CARD_POLL_TX_ENTER_READ);
        }
        break;
    }
    case FM17660K_CARD_POLL_TX_ENTER_READ:
        rc = fm17660k_read(
            fm17660k, H2_FM17660K_REG_PAGE_SELECT,
            &session->poll_page_value);
        if (rc == H2_PAL_OK) {
            fm17660k_card_poll_set_phase(
                fm17660k, session, FM17660K_CARD_POLL_TX_ENTER_WRITE);
        }
        break;
    case FM17660K_CARD_POLL_TX_ENTER_WRITE:
        rc = fm17660k_write(
            fm17660k, H2_FM17660K_REG_PAGE_SELECT,
            (uint8_t)(session->poll_page_value | 0x40u));
        if (rc == H2_PAL_OK) {
            fm17660k_card_poll_set_phase(
                fm17660k, session, FM17660K_CARD_POLL_TX_COMMAND);
        }
        break;
    case FM17660K_CARD_POLL_TX_COMMAND:
        rc = fm17660k_write(
            fm17660k, H2_FM17660K_REG_RF_COMMAND,
            H2_FM17660K_RF_COMMAND_TX_FULL);
        if (rc == H2_PAL_OK) {
            fm17660k_card_poll_set_phase(
                fm17660k, session, FM17660K_CARD_POLL_TX_LEAVE_READ);
        }
        break;
    case FM17660K_CARD_POLL_TX_LEAVE_READ:
        rc = fm17660k_read(
            fm17660k, H2_FM17660K_REG_PAGE_SELECT,
            &session->poll_page_value);
        if (rc == H2_PAL_OK) {
            fm17660k_card_poll_set_phase(
                fm17660k, session, FM17660K_CARD_POLL_TX_LEAVE_WRITE);
        }
        break;
    case FM17660K_CARD_POLL_TX_LEAVE_WRITE:
        rc = fm17660k_write(
            fm17660k, H2_FM17660K_REG_PAGE_SELECT,
            (uint8_t)(session->poll_page_value & 0xbfu));
        if (rc == H2_PAL_OK) {
            fm17660k_card_poll_set_phase(
                fm17660k, session, FM17660K_CARD_POLL_POSTPROCESS);
        }
        break;
    case FM17660K_CARD_POLL_POSTPROCESS:
        if ((session->poll_main_irq & H2_FM17660K_CARD_IRQ_TX_END) != 0u &&
            session->mode == H2_PAL_NFC_CARD_EMULATION_MODE_RAW_FRAME) {
            (void)fm17660k_event_push(
                session,
                H2_PAL_NFC_CARD_EMULATION_EVENT_RAW_FRAME_TRANSMITTED,
                H2_PAL_OK,
                session->tx_frame,
                session->tx_payload_bit_len);
        }
        if ((session->poll_aux_irq &
             (H2_FM17660K_CARD_AUX_PARITY |
              H2_FM17660K_CARD_AUX_CRC |
              H2_FM17660K_CARD_AUX_COMMUNICATION)) != 0u) {
            (void)fm17660k_event_push(
                session, H2_PAL_NFC_CARD_EMULATION_EVENT_ERROR,
                H2_PAL_ERR_FORMAT, NULL, 0u);
        }
        if ((session->poll_main_irq & H2_FM17660K_CARD_IRQ_RF_OFF) != 0u) {
            if (session->active) {
                if (session->type2 != NULL) {
                    (void)h2_nfc_type2_deactivate(session->type2);
                }
                session->active = 0;
                (void)fm17660k_event_push(
                    session, H2_PAL_NFC_CARD_EMULATION_EVENT_DEACTIVATED,
                    H2_PAL_OK, NULL, 0u);
            }
            (void)fm17660k_event_push(
                session, H2_PAL_NFC_CARD_EMULATION_EVENT_FIELD_OFF,
                H2_PAL_OK, NULL, 0u);
            fm17660k_card_poll_set_phase(
                fm17660k, session, FM17660K_CARD_POLL_REARM_FLUSH_READ);
        } else {
            fm17660k_card_poll_set_phase(
                fm17660k, session, FM17660K_CARD_POLL_COMPLETE);
        }
        break;
    case FM17660K_CARD_POLL_REARM_FLUSH_READ:
        rc = fm17660k_read(
            fm17660k, H2_FM17660K_REG_FIFO_CONTROL,
            &session->poll_fifo_control);
        if (rc == H2_PAL_OK) {
            fm17660k_card_poll_set_phase(
                fm17660k, session, FM17660K_CARD_POLL_REARM_FLUSH_WRITE);
        }
        break;
    case FM17660K_CARD_POLL_REARM_FLUSH_WRITE:
        rc = fm17660k_write(
            fm17660k, H2_FM17660K_REG_FIFO_CONTROL,
            (uint8_t)(session->poll_fifo_control | 0x10u));
        if (rc == H2_PAL_OK) {
            fm17660k_card_poll_set_phase(
                fm17660k, session, FM17660K_CARD_POLL_REARM_ENTER_READ);
        }
        break;
    case FM17660K_CARD_POLL_REARM_ENTER_READ:
        rc = fm17660k_read(
            fm17660k, H2_FM17660K_REG_PAGE_SELECT,
            &session->poll_page_value);
        if (rc == H2_PAL_OK) {
            fm17660k_card_poll_set_phase(
                fm17660k, session, FM17660K_CARD_POLL_REARM_ENTER_WRITE);
        }
        break;
    case FM17660K_CARD_POLL_REARM_ENTER_WRITE:
        rc = fm17660k_write(
            fm17660k, H2_FM17660K_REG_PAGE_SELECT,
            (uint8_t)(session->poll_page_value | 0x40u));
        if (rc == H2_PAL_OK) {
            fm17660k_card_poll_set_phase(
                fm17660k, session, FM17660K_CARD_POLL_REARM_MAIN_IRQ);
        }
        break;
    case FM17660K_CARD_POLL_REARM_MAIN_IRQ:
        rc = fm17660k_write(
            fm17660k, H2_FM17660K_REG_CARD_MAIN_IRQ, 0x00u);
        if (rc == H2_PAL_OK) {
            fm17660k_card_poll_set_phase(
                fm17660k, session, FM17660K_CARD_POLL_REARM_AUX_IRQ);
        }
        break;
    case FM17660K_CARD_POLL_REARM_AUX_IRQ:
        rc = fm17660k_write(
            fm17660k, H2_FM17660K_REG_CARD_AUX_IRQ, 0x00u);
        if (rc == H2_PAL_OK) {
            fm17660k_card_poll_set_phase(
                fm17660k, session, FM17660K_CARD_POLL_REARM_NC_MODE);
        }
        break;
    case FM17660K_CARD_POLL_REARM_NC_MODE:
        rc = fm17660k_write(
            fm17660k, H2_FM17660K_REG_NC_MODE, 0x80u);
        if (rc == H2_PAL_OK) {
            fm17660k_card_poll_set_phase(
                fm17660k, session, FM17660K_CARD_POLL_REARM_RF_COMMAND);
        }
        break;
    case FM17660K_CARD_POLL_REARM_RF_COMMAND:
        rc = fm17660k_write(
            fm17660k, H2_FM17660K_REG_RF_COMMAND,
            H2_FM17660K_RF_COMMAND_RX);
        if (rc == H2_PAL_OK) {
            fm17660k_card_poll_set_phase(
                fm17660k, session, FM17660K_CARD_POLL_REARM_LEAVE_READ);
        }
        break;
    case FM17660K_CARD_POLL_REARM_LEAVE_READ:
        rc = fm17660k_read(
            fm17660k, H2_FM17660K_REG_PAGE_SELECT,
            &session->poll_page_value);
        if (rc == H2_PAL_OK) {
            fm17660k_card_poll_set_phase(
                fm17660k, session, FM17660K_CARD_POLL_REARM_LEAVE_WRITE);
        }
        break;
    case FM17660K_CARD_POLL_REARM_LEAVE_WRITE:
        rc = fm17660k_write(
            fm17660k, H2_FM17660K_REG_PAGE_SELECT,
            (uint8_t)(session->poll_page_value & 0xbfu));
        if (rc == H2_PAL_OK) {
            fm17660k_card_poll_set_phase(
                fm17660k, session, FM17660K_CARD_POLL_COMPLETE);
        }
        break;
    case FM17660K_CARD_POLL_COMPLETE:
    case FM17660K_CARD_POLL_IDLE:
    default:
        break;
    }

    if (rc == H2_PAL_ERR_WOULD_BLOCK) {
        return fm17660k_card_poll_pending_result(fm17660k, session);
    }
    return rc;
}

static h2_pal_result_t fm17660k_card_poll(
    void *user,
    fm17660k_card_session_t *session,
    fm17660k_card_event_t *out_event) {
    h2_fm17660k_t *fm17660k = user;
    if (fm17660k == NULL || session != &fm17660k->session || !session->open ||
        !session->started) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (session->poll_phase == FM17660K_CARD_POLL_IDLE) {
        h2_pal_result_t event_rc = fm17660k_event_pop(session, out_event);
        if (event_rc == H2_PAL_OK) {
            return H2_PAL_OK;
        }
        fm17660k_card_poll_set_phase(
            fm17660k, session, FM17660K_CARD_POLL_IRQ_ENTER_READ);
    }

    h2_pal_result_t rc = H2_PAL_OK;
    for (size_t progress = 0u;
         progress < FM17660K_CARD_POLL_PROGRESS_LIMIT &&
         rc == H2_PAL_OK &&
         session->poll_phase != FM17660K_CARD_POLL_COMPLETE;
         ++progress) {
        rc = fm17660k_card_poll_step(fm17660k, session);
    }
    if (rc != H2_PAL_OK && rc != H2_PAL_ERR_WOULD_BLOCK) {
        (void)fm17660k_event_push(
            session, H2_PAL_NFC_CARD_EMULATION_EVENT_ERROR, rc, NULL, 0u);
        fm17660k_card_poll_set_phase(
            fm17660k, session, FM17660K_CARD_POLL_COMPLETE);
    }
    if (session->poll_phase == FM17660K_CARD_POLL_COMPLETE) {
        session->poll_phase = FM17660K_CARD_POLL_IDLE;
        session->poll_fifo_offset = 0u;
        session->poll_frame_length = 0u;
        session->poll_frame_crc_valid = 0;
    }
    h2_pal_result_t event_rc = fm17660k_event_pop(session, out_event);
    if (event_rc == H2_PAL_OK) {
        return H2_PAL_OK;
    }
    return rc == H2_PAL_OK ? H2_PAL_ERR_WOULD_BLOCK : rc;
}

static h2_pal_result_t fm17660k_card_stop(
    void *user,
    fm17660k_card_session_t *session) {
    h2_fm17660k_t *fm17660k = user;
    if (fm17660k == NULL || session != &fm17660k->session || !session->open) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (!session->started) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (session->active || session->poll_phase != FM17660K_CARD_POLL_IDLE) {
        return H2_PAL_ERR_BUSY;
    }
    h2_pal_result_t rc = fm17660k_ext_enter(fm17660k);
    int extended_page = rc == H2_PAL_OK;
    if (rc == H2_PAL_OK) {
        rc = fm17660k_write(
            fm17660k, H2_FM17660K_REG_RF_COMMAND,
            H2_FM17660K_RF_COMMAND_IDLE);
    }
    if (rc == H2_PAL_OK) {
        rc = fm17660k_write(fm17660k, H2_FM17660K_REG_NC_MODE, 0x00u);
    }
    if (rc == H2_PAL_OK) {
        rc = fm17660k_write(
            fm17660k, H2_FM17660K_REG_CARD_CONTROL, 0x01u);
    }
    if (rc == H2_PAL_OK) {
        rc = fm17660k_sleep(fm17660k, 1u);
        if (rc == H2_PAL_OK) {
            rc = fm17660k_write(
                fm17660k, H2_FM17660K_REG_CARD_CONTROL, 0x00u);
        }
    }
    if (extended_page) {
        h2_pal_result_t leave_rc = fm17660k_ext_leave(fm17660k);
        if (rc == H2_PAL_OK) {
            rc = leave_rc;
        }
    }
    if (rc == H2_PAL_OK) {
        session->started = 0;
        session->event_head = 0u;
        session->event_count = 0u;
    }
    return rc;
}

static h2_pal_result_t fm17660k_card_close(
    void *user,
    fm17660k_card_session_t *session) {
    h2_fm17660k_t *fm17660k = user;
    if (fm17660k == NULL || session != &fm17660k->session) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (!session->open) {
        return H2_PAL_OK;
    }
    if (session->started || session->active) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    h2_nfc_type2_destroy(session->type2);
    memset(session, 0, sizeof(*session));
    fm17660k->frontend_owner = FM17660K_FRONTEND_NONE;
    return H2_PAL_OK;
}

static h2_pal_result_t fm17660k_card_emulate(
    void *user,
    const h2_pal_nfc_card_emulation_config_t *config,
    h2_pal_nfc_card_emulation_result_t *out_result) {
    h2_fm17660k_t *fm17660k = user;
    if (config == NULL || out_result == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_result, 0, sizeof(*out_result));
    if (fm17660k == NULL || !fm17660k->initialized) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    fm17660k_card_session_t *session = NULL;
    h2_pal_nfc_card_emulation_result_t result = {
        .completion = H2_PAL_NFC_CARD_EMULATION_COMPLETION_WINDOW_EXPIRED,
        .content_revision = config->content_revision,
    };
    const fm17660k_card_open_config_t open_config = {
        .periph_id = config->periph_id,
        .technology = config->technology,
        .mode = config->mode,
        .managed_profile = config->managed_profile,
        .uid = config->uid,
        .uid_len = config->uid_len,
    };
    const fm17660k_card_content_t content = {
        .mode = config->mode,
        .revision = config->content_revision,
        .managed_bytes = config->managed_bytes,
        .managed_len = config->managed_len,
        .raw_exchange = config->raw_exchange,
        .raw_exchange_user = config->raw_exchange_user,
    };

    h2_pal_result_t rc = fm17660k_card_open(
        user, &open_config, &session);
    if (rc == H2_PAL_OK) {
        rc = fm17660k_card_set_content(user, session, &content);
    }
    if (rc == H2_PAL_OK) {
        rc = fm17660k_card_start(user, session);
    }

    uint64_t start_ms = fm17660k->transport.vtable->now_ms(
        fm17660k->transport.user);
    int completed = 0;
    while (rc == H2_PAL_OK && !completed) {
        fm17660k_card_event_t event;
        h2_pal_result_t poll_rc = fm17660k_card_poll(user, session, &event);
        if (poll_rc == H2_PAL_OK) {
            switch (event.type) {
            case H2_PAL_NFC_CARD_EMULATION_EVENT_FIELD_ON:
                result.field_detected = 1;
                break;
            case H2_PAL_NFC_CARD_EMULATION_EVENT_ACTIVATED:
                result.activated = 1;
                break;
            case H2_PAL_NFC_CARD_EMULATION_EVENT_CONTENT_ACCESSED:
                result.completion =
                    H2_PAL_NFC_CARD_EMULATION_COMPLETION_CONTENT_ACCESSED;
                result.content_revision = event.content_revision;
                completed = 1;
                break;
            case H2_PAL_NFC_CARD_EMULATION_EVENT_DEACTIVATED:
                result.completion =
                    H2_PAL_NFC_CARD_EMULATION_COMPLETION_DEACTIVATED;
                result.content_revision = event.content_revision;
                completed = 1;
                break;
            case H2_PAL_NFC_CARD_EMULATION_EVENT_ERROR:
                rc = event.result == H2_PAL_OK ? H2_PAL_ERR_IO : event.result;
                break;
            default:
                break;
            }
        } else if (poll_rc != H2_PAL_ERR_WOULD_BLOCK) {
            rc = poll_rc;
        }
        if (rc != H2_PAL_OK || completed) {
            continue;
        }
        uint64_t now_ms = fm17660k->transport.vtable->now_ms(
            fm17660k->transport.user);
        if (config->window_ms != H2_PAL_NFC_CARD_EMULATION_WAIT_FOREVER &&
            now_ms - start_ms >= config->window_ms) {
            completed = 1;
            continue;
        }
        rc = fm17660k_sleep(fm17660k, 1u);
    }

    if (session != NULL && session->open) {
        if (session->active && session->type2 != NULL) {
            h2_pal_result_t deactivate_rc =
                h2_nfc_type2_deactivate(session->type2);
            if (rc == H2_PAL_OK) {
                rc = deactivate_rc;
            }
        }
        session->active = 0;
        session->poll_phase = FM17660K_CARD_POLL_IDLE;
        if (session->started) {
            h2_pal_result_t stop_rc = fm17660k_card_stop(user, session);
            if (rc == H2_PAL_OK) {
                rc = stop_rc;
            }
        }
        h2_pal_result_t close_rc = fm17660k_card_close(user, session);
        if (rc == H2_PAL_OK) {
            rc = close_rc;
        }
    }
    if (rc == H2_PAL_OK) {
        *out_result = result;
    }
    return rc;
}

static const h2_pal_nfc_vtable_t fm17660k_reader_vtable = {
    .scan_nfc_reader = fm17660k_reader_scan,
    .read_nfc_data = fm17660k_reader_read,
};

static const h2_pal_nfc_card_emulation_vtable_t fm17660k_card_vtable = {
    .get_capabilities = fm17660k_card_get_capabilities,
    .emulate = fm17660k_card_emulate,
};

h2_pal_result_t h2_fm17660k_init(
    const h2_fm17660k_config_t *config,
    h2_fm17660k_t **out_fm17660k) {
    if (out_fm17660k == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_fm17660k = NULL;
    if (config == NULL || config->mem == NULL ||
        fm17660k_transport_is_valid(&config->transport) != H2_PAL_OK) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_fm17660k_t *fm17660k =
        h2_pal_mem_alloc(config->mem, sizeof(*fm17660k));
    if (fm17660k == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(fm17660k, 0, sizeof(*fm17660k));
    fm17660k->mem = config->mem;
    fm17660k->periph_id = config->periph_id;
    fm17660k->transport = config->transport;
    fm17660k->timeout_ms = config->operation_timeout_ms == 0u ?
        FM17660K_DEFAULT_TIMEOUT_MS : config->operation_timeout_ms;
    fm17660k->reader_api = (h2_pal_nfc_api_t){
        .user = fm17660k,
        .vtable = &fm17660k_reader_vtable,
    };
    fm17660k->card_api = (h2_pal_nfc_card_emulation_api_t){
        .user = fm17660k,
        .vtable = &fm17660k_card_vtable,
    };
    h2_pal_result_t rc = fm17660k_validate_controller(fm17660k);
    if (rc != H2_PAL_OK) {
        memset(fm17660k, 0, sizeof(*fm17660k));
        h2_pal_mem_free(config->mem, fm17660k);
        return rc;
    }
    fm17660k->initialized = 1;
    *out_fm17660k = fm17660k;
    return H2_PAL_OK;
}

void h2_fm17660k_deinit(h2_fm17660k_t *fm17660k) {
    if (fm17660k == NULL) {
        return;
    }
    if (fm17660k->session.open) {
        if (fm17660k->session.started) {
            (void)fm17660k_write(
                fm17660k,
                H2_FM17660K_REG_COMMAND,
                H2_FM17660K_COMMAND_IDLE);
            if (fm17660k_ext_enter(fm17660k) == H2_PAL_OK) {
                (void)fm17660k_write(
                    fm17660k, H2_FM17660K_REG_NC_MODE, 0x00u);
                (void)fm17660k_write(
                    fm17660k, H2_FM17660K_REG_CARD_CONTROL, 0x01u);
                (void)fm17660k_sleep(fm17660k, 1u);
                (void)fm17660k_write(
                    fm17660k, H2_FM17660K_REG_CARD_CONTROL, 0x00u);
                (void)fm17660k_ext_leave(fm17660k);
            }
        }
        if (fm17660k->session.active && fm17660k->session.type2 != NULL) {
            (void)h2_nfc_type2_deactivate(fm17660k->session.type2);
        }
        h2_nfc_type2_destroy(fm17660k->session.type2);
    }
    const h2_pal_mem_api_t *mem = fm17660k->mem;
    memset(fm17660k, 0, sizeof(*fm17660k));
    h2_pal_mem_free(mem, fm17660k);
}

const h2_pal_nfc_api_t *h2_fm17660k_reader_api(
    h2_fm17660k_t *fm17660k) {
    return fm17660k != NULL && fm17660k->initialized ?
        &fm17660k->reader_api : NULL;
}

const h2_pal_nfc_card_emulation_api_t *h2_fm17660k_card_emulation_api(
    h2_fm17660k_t *fm17660k) {
    return fm17660k != NULL && fm17660k->initialized ?
        &fm17660k->card_api : NULL;
}

h2_pal_result_t h2_fm17660k_recover(h2_fm17660k_t *fm17660k) {
    if (fm17660k == NULL || !fm17660k->initialized) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (fm17660k->frontend_owner != FM17660K_FRONTEND_NONE) {
        return H2_PAL_ERR_BUSY;
    }
    return fm17660k_validate_controller(fm17660k);
}
