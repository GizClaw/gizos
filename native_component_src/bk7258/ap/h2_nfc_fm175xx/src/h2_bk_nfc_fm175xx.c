#include "h2_bk_nfc_fm175xx.h"

#include <common/bk_err.h>
#include <driver/i2c.h>
#include <os/os.h>
#include <string.h>

#define H2_BK_NFC_I2C_TIMEOUT_MS 20u
#define H2_BK_NFC_I2C_WRITE_MAX 32u

static h2_pal_result_t pal_rc_from_fm(int rc) {
    switch (rc) {
    case H2_FM175XX_OK:
        return H2_PAL_OK;
    case H2_FM175XX_ERR_INVALID_ARG:
        return H2_PAL_ERR_INVALID_ARG;
    case H2_FM175XX_ERR_UNAVAILABLE:
        return H2_PAL_ERR_UNAVAILABLE;
    case H2_FM175XX_ERR_NO_MEMORY:
        return H2_PAL_ERR_NO_MEMORY;
    case H2_FM175XX_ERR_TIMEOUT:
        return H2_PAL_ERR_TIMEOUT;
    case H2_FM175XX_ERR_IO:
    case H2_FM175XX_ERR_PROTOCOL:
    default:
        return H2_PAL_ERR_IO;
    }
}

static int fm_i2c_write_regs(
    void *user,
    uint8_t reg,
    const uint8_t *data,
    size_t len) {
    h2_bk_nfc_fm175xx_t *adapter = user;
    uint8_t buffer[H2_BK_NFC_I2C_WRITE_MAX + 1u];

    if (adapter == NULL || (data == NULL && len != 0u) ||
        len > H2_BK_NFC_I2C_WRITE_MAX) {
        return H2_FM175XX_ERR_INVALID_ARG;
    }
    buffer[0] = reg;
    if (len != 0u) {
        memcpy(buffer + 1u, data, len);
    }
    return bk_i2c_master_write(
               adapter->config.i2c_id,
               adapter->config.i2c_address,
               buffer,
               (uint32_t)len + 1u,
               H2_BK_NFC_I2C_TIMEOUT_MS) == BK_OK ?
        H2_FM175XX_OK : H2_FM175XX_ERR_IO;
}

static int fm_i2c_write_reg(void *user, uint8_t reg, uint8_t value) {
    return fm_i2c_write_regs(user, reg, &value, 1u);
}

static int fm_i2c_read_regs(
    void *user,
    uint8_t reg,
    uint8_t *out_data,
    size_t len) {
    h2_bk_nfc_fm175xx_t *adapter = user;
    if (adapter == NULL || (out_data == NULL && len != 0u)) {
        return H2_FM175XX_ERR_INVALID_ARG;
    }
    i2c_mem_param_t params = {
        .dev_addr = adapter->config.i2c_address,
        .mem_addr = reg,
        .mem_addr_size = I2C_MEM_ADDR_SIZE_8BIT,
        .data = out_data,
        .data_size = (uint32_t)len,
        .timeout_ms = H2_BK_NFC_I2C_TIMEOUT_MS,
    };
    return bk_i2c_memory_read(adapter->config.i2c_id, &params) == BK_OK ?
        H2_FM175XX_OK : H2_FM175XX_ERR_IO;
}

static int fm_i2c_read_reg(void *user, uint8_t reg, uint8_t *out_value) {
    return fm_i2c_read_regs(user, reg, out_value, 1u);
}

static void fm_i2c_sleep_ms(void *user, uint32_t ms) {
    (void)user;
    rtos_delay_milliseconds(ms);
}

static h2_pal_result_t ensure_open(h2_bk_nfc_fm175xx_t *adapter) {
    if (adapter == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (adapter->opened != 0) {
        return H2_PAL_OK;
    }
    const i2c_config_t config = {
        .baud_rate = adapter->config.i2c_speed_hz,
        .addr_mode = I2C_ADDR_MODE_7BIT,
        .slave_addr = 0u,
    };
    if (bk_i2c_driver_init() != BK_OK ||
        bk_i2c_init(adapter->config.i2c_id, &config) != BK_OK) {
        return H2_PAL_ERR_IO;
    }
    const h2_fm175xx_transport_t transport = {
        .user = adapter,
        .write_reg = fm_i2c_write_reg,
        .write_regs = fm_i2c_write_regs,
        .read_reg = fm_i2c_read_reg,
        .read_regs = fm_i2c_read_regs,
        .sleep_ms = fm_i2c_sleep_ms,
    };
    int rc = h2_fm175xx_init(&adapter->reader, &transport);
    if (rc == H2_FM175XX_OK) {
        rc = h2_fm175xx_open_type_a(&adapter->reader);
    }
    if (rc != H2_FM175XX_OK) {
        (void)bk_i2c_deinit(adapter->config.i2c_id);
        return pal_rc_from_fm(rc);
    }
    adapter->opened = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t scan_nfc_reader(
    void *user,
    h2_pal_periph_id_t id,
    h2_pal_nfc_scan_t *out_scan) {
    h2_bk_nfc_fm175xx_t *adapter = user;
    if (adapter == NULL || out_scan == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (id != adapter->config.id) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    memset(out_scan, 0, sizeof(*out_scan));
    out_scan->id = id;
    out_scan->stage = H2_PAL_NFC_STAGE_ERROR;
    h2_pal_result_t rc = ensure_open(adapter);
    if (rc != H2_PAL_OK) {
        out_scan->result = rc;
        return rc;
    }
    h2_fm175xx_type_a_card_t card;
    int fm_rc = h2_fm175xx_type_a_activate(&adapter->reader, &card);
    if (fm_rc == H2_FM175XX_ERR_TIMEOUT) {
        out_scan->stage = H2_PAL_NFC_STAGE_ABSENT;
        out_scan->result = H2_PAL_OK;
        return H2_PAL_OK;
    }
    if (fm_rc != H2_FM175XX_OK) {
        out_scan->result = pal_rc_from_fm(fm_rc);
        return out_scan->result;
    }
    out_scan->stage = H2_PAL_NFC_STAGE_DISCOVERED;
    out_scan->result = H2_PAL_OK;
    out_scan->tag_type = H2_PAL_NFC_TAG_TYPE_ISO14443A;
    out_scan->uid_len = card.uid_len > H2_PAL_NFC_UID_MAX_LEN ?
        H2_PAL_NFC_UID_MAX_LEN : card.uid_len;
    memcpy(out_scan->uid, card.uid, out_scan->uid_len);
    return H2_PAL_OK;
}

static int uid_matches(
    const h2_fm175xx_type_a_card_t *card,
    const uint8_t *expected_uid,
    uint8_t expected_uid_len) {
    return expected_uid_len == 0u ||
        (card->uid_len == expected_uid_len &&
         memcmp(card->uid, expected_uid, expected_uid_len) == 0);
}

static h2_pal_result_t read_nfc_data(
    void *user,
    h2_pal_periph_id_t id,
    const uint8_t *expected_uid,
    uint8_t expected_uid_len,
    h2_pal_nfc_data_type_t requested_type,
    const h2_pal_mem_api_t *allocator,
    h2_pal_nfc_data_read_t *out_data) {
    h2_bk_nfc_fm175xx_t *adapter = user;
    if (adapter == NULL || allocator == NULL || out_data == NULL ||
        (expected_uid == NULL && expected_uid_len != 0u) ||
        expected_uid_len > H2_PAL_NFC_UID_MAX_LEN) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_data, 0, sizeof(*out_data));
    if (id != adapter->config.id) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    if (requested_type != H2_PAL_NFC_DATA_NTAG_PAGES) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    h2_pal_result_t rc = ensure_open(adapter);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    h2_fm175xx_type_a_card_t card;
    int fm_rc = h2_fm175xx_type_a_activate(&adapter->reader, &card);
    if (fm_rc == H2_FM175XX_ERR_TIMEOUT) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    if (fm_rc != H2_FM175XX_OK) {
        return pal_rc_from_fm(fm_rc);
    }
    if (!uid_matches(&card, expected_uid, expected_uid_len)) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    size_t capacity = adapter->config.read_capacity != 0u ?
        adapter->config.read_capacity :
        H2_BK_NFC_FM175XX_DEFAULT_READ_CAPACITY;
    uint8_t *bytes = h2_pal_mem_alloc(allocator, capacity);
    if (bytes == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    size_t len = 0u;
    fm_rc = h2_fm175xx_ntag_read_all(
        &adapter->reader,
        bytes,
        capacity,
        &len);
    if (fm_rc != H2_FM175XX_OK) {
        h2_pal_mem_free(allocator, bytes);
        return pal_rc_from_fm(fm_rc);
    }
    out_data->id = id;
    out_data->tag_type = H2_PAL_NFC_TAG_TYPE_NTAG;
    out_data->uid_len = card.uid_len > H2_PAL_NFC_UID_MAX_LEN ?
        H2_PAL_NFC_UID_MAX_LEN : card.uid_len;
    memcpy(out_data->uid, card.uid, out_data->uid_len);
    out_data->type = H2_PAL_NFC_DATA_NTAG_PAGES;
    out_data->bytes = bytes;
    out_data->len = len;
    return H2_PAL_OK;
}

h2_pal_result_t h2_bk_nfc_fm175xx_init(
    h2_bk_nfc_fm175xx_t *adapter,
    const h2_bk_nfc_fm175xx_config_t *config) {
    static const h2_pal_nfc_vtable_t vtable = {
        .scan_nfc_reader = scan_nfc_reader,
        .read_nfc_data = read_nfc_data,
    };
    if (adapter == NULL || config == NULL || config->id == 0u ||
        config->i2c_address == 0u || config->i2c_speed_hz == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(adapter, 0, sizeof(*adapter));
    adapter->config = *config;
    adapter->api.user = adapter;
    adapter->api.vtable = &vtable;
    return H2_PAL_OK;
}

const h2_pal_nfc_api_t *h2_bk_nfc_fm175xx_api(
    h2_bk_nfc_fm175xx_t *adapter) {
    return adapter == NULL ? NULL : &adapter->api;
}
