#include "h2_esp_nfc_fm175xx.h"
#include "h2_esp_nfc_fm175xx_internal.h"

#include <string.h>

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

static h2_pal_result_t ensure_open(h2_esp_nfc_fm175xx_t *adapter) {
    if (adapter == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (adapter->opened != 0) {
        return H2_PAL_OK;
    }
    h2_fm175xx_transport_t transport;
    memset(&transport, 0, sizeof(transport));
    if (adapter->transport_kind == H2_ESP_NFC_FM175XX_TRANSPORT_SPI) {
        if (h2_esp_nfc_fm175xx_spi_open(adapter, &transport) != H2_FM175XX_OK) {
            return H2_PAL_ERR_IO;
        }
    } else {
        if (h2_esp_nfc_fm175xx_i2c_open(adapter, &transport) != H2_FM175XX_OK) {
            return H2_PAL_ERR_IO;
        }
    }
    int rc = h2_fm175xx_init(&adapter->reader, &transport);
    if (rc == H2_FM175XX_OK) {
        rc = h2_fm175xx_open_type_a(&adapter->reader);
    }
    if (rc != H2_FM175XX_OK) {
        h2_esp_nfc_fm175xx_deinit(adapter);
        return pal_rc_from_fm(rc);
    }

    adapter->opened = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t scan_nfc_reader(
    void *user,
    h2_pal_periph_id_t id,
    h2_pal_nfc_scan_t *out_scan) {
    h2_esp_nfc_fm175xx_t *adapter = (h2_esp_nfc_fm175xx_t *)user;
    if (adapter == NULL || out_scan == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (id != adapter->config.id) {
        return H2_PAL_ERR_NOT_FOUND;
    }

    memset(out_scan, 0, sizeof(*out_scan));
    out_scan->id = id;
    out_scan->stage = H2_PAL_NFC_STAGE_ERROR;
    out_scan->result = H2_PAL_OK;

    h2_pal_result_t prc = ensure_open(adapter);
    if (prc != H2_PAL_OK) {
        out_scan->result = prc;
        return prc;
    }

    h2_fm175xx_type_a_card_t card;
    const int rc = h2_fm175xx_type_a_activate(&adapter->reader, &card);
    if (rc == H2_FM175XX_ERR_TIMEOUT) {
        out_scan->stage = H2_PAL_NFC_STAGE_ABSENT;
        out_scan->result = H2_PAL_OK;
        out_scan->tag_type = H2_PAL_NFC_TAG_TYPE_UNKNOWN;
        return H2_PAL_OK;
    }
    if (rc != H2_FM175XX_OK) {
        out_scan->stage = H2_PAL_NFC_STAGE_ERROR;
        out_scan->result = pal_rc_from_fm(rc);
        out_scan->tag_type = H2_PAL_NFC_TAG_TYPE_UNKNOWN;
        return out_scan->result;
    }

    out_scan->stage = H2_PAL_NFC_STAGE_DISCOVERED;
    out_scan->result = H2_PAL_OK;
    out_scan->tag_type = H2_PAL_NFC_TAG_TYPE_ISO14443A;
    out_scan->uid_len = card.uid_len > H2_PAL_NFC_UID_MAX_LEN
        ? H2_PAL_NFC_UID_MAX_LEN
        : card.uid_len;
    memcpy(out_scan->uid, card.uid, out_scan->uid_len);
    return H2_PAL_OK;
}

static int uid_matches(
    const h2_fm175xx_type_a_card_t *card,
    const uint8_t *expected_uid,
    uint8_t expected_uid_len) {
    if (expected_uid_len == 0u) {
        return 1;
    }
    return card->uid_len == expected_uid_len &&
           memcmp(card->uid, expected_uid, expected_uid_len) == 0;
}

static h2_pal_result_t read_nfc_data(
    void *user,
    h2_pal_periph_id_t id,
    const uint8_t *expected_uid,
    uint8_t expected_uid_len,
    h2_pal_nfc_data_type_t requested_type,
    const h2_pal_mem_api_t *allocator,
    h2_pal_nfc_data_read_t *out_data) {
    h2_esp_nfc_fm175xx_t *adapter = (h2_esp_nfc_fm175xx_t *)user;
    if (out_data == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_data, 0, sizeof(*out_data));

    if (adapter == NULL || allocator == NULL ||
        (expected_uid == NULL && expected_uid_len != 0u) ||
        expected_uid_len > H2_PAL_NFC_UID_MAX_LEN) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (id != adapter->config.id) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    if (requested_type != H2_PAL_NFC_DATA_NTAG_PAGES) {
        return H2_PAL_ERR_UNSUPPORTED;
    }

    h2_pal_result_t prc = ensure_open(adapter);
    if (prc != H2_PAL_OK) {
        return prc;
    }

    h2_fm175xx_type_a_card_t card;
    int rc = h2_fm175xx_type_a_activate(&adapter->reader, &card);
    if (rc == H2_FM175XX_ERR_TIMEOUT) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    if (rc != H2_FM175XX_OK) {
        return pal_rc_from_fm(rc);
    }
    if (!uid_matches(&card, expected_uid, expected_uid_len)) {
        return H2_PAL_ERR_INVALID_STATE;
    }

    size_t capacity = adapter->config.read_capacity;
    if (capacity == 0u) {
        capacity = H2_ESP_NFC_FM175XX_DEFAULT_READ_CAPACITY;
    }
    uint8_t *bytes = (uint8_t *)h2_pal_mem_alloc(allocator, capacity);
    if (bytes == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }

    size_t len = 0u;
    rc = h2_fm175xx_ntag_read_all(&adapter->reader, bytes, capacity, &len);
    if (rc != H2_FM175XX_OK) {
        h2_pal_mem_free(allocator, bytes);
        return pal_rc_from_fm(rc);
    }

    out_data->id = id;
    out_data->tag_type = H2_PAL_NFC_TAG_TYPE_NTAG;
    out_data->uid_len = card.uid_len > H2_PAL_NFC_UID_MAX_LEN
        ? H2_PAL_NFC_UID_MAX_LEN
        : card.uid_len;
    memcpy(out_data->uid, card.uid, out_data->uid_len);
    out_data->type = H2_PAL_NFC_DATA_NTAG_PAGES;
    out_data->bytes = bytes;
    out_data->len = len;
    return H2_PAL_OK;
}

const h2_pal_nfc_vtable_t h2_esp_nfc_fm175xx_nfc_vtable = {
    .scan_nfc_reader = scan_nfc_reader,
    .read_nfc_data = read_nfc_data,
};

h2_pal_result_t h2_esp_nfc_fm175xx_init(
    h2_esp_nfc_fm175xx_t *adapter,
    const h2_esp_nfc_fm175xx_config_t *config) {
    if (adapter == NULL || config == NULL ||
        config->id == 0u || config->bus == NULL ||
        config->i2c_address == 0u || config->i2c_speed_hz == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(adapter, 0, sizeof(*adapter));
    adapter->config = *config;
    adapter->transport_kind = H2_ESP_NFC_FM175XX_TRANSPORT_I2C;
    adapter->api.user = adapter;
    adapter->api.vtable = &h2_esp_nfc_fm175xx_nfc_vtable;
    return H2_PAL_OK;
}

void h2_esp_nfc_fm175xx_deinit(h2_esp_nfc_fm175xx_t *adapter) {
    if (adapter == NULL) {
        return;
    }
    if (adapter->transport_kind == H2_ESP_NFC_FM175XX_TRANSPORT_SPI) {
        h2_esp_nfc_fm175xx_spi_close(adapter);
    } else {
        h2_esp_nfc_fm175xx_i2c_close(adapter);
    }
    memset(&adapter->reader, 0, sizeof(adapter->reader));
    adapter->opened = 0;
}

const h2_pal_nfc_api_t *h2_esp_nfc_fm175xx_api(h2_esp_nfc_fm175xx_t *adapter) {
    return adapter == NULL ? NULL : &adapter->api;
}
