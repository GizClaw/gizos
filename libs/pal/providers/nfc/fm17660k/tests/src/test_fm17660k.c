#include "h2_fm17660k.h"

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define REG_COMMAND 0x00u
#define REG_FIFO_CONTROL 0x02u
#define REG_FIFO_LENGTH 0x04u
#define REG_IRQ0 0x06u
#define REG_ERROR 0x0au
#define REG_TX_CRC_CONTROL 0x2cu
#define REG_RX_CRC_CONTROL 0x2du
#define REG_CARD_MAIN_IRQ 0x57u
#define REG_VERSION 0x7fu
#define REG_FIFO_DATA 0x05u

typedef struct fake_transport {
    uint8_t registers[128];
    uint8_t rx_fifo[512];
    size_t rx_len;
    size_t rx_offset;
    uint8_t tx_fifo[1024];
    size_t tx_len;
    size_t reset_calls;
    size_t write_calls;
    size_t fail_write_on_call;
    uint64_t now_ms;
    bool reader_script_enabled;
    bool reader_four_byte_uid;
    bool reader_cl3_enabled;
    bool reader_rx_with_timer_irq;
    bool reader_irq_transport_timeout;
    bool reader_controller_error;
    bool reader_short_atqa;
    bool reader_bad_bcc;
    bool reader_bad_sak_cascade;
    bool reader_uid_length_mismatch;
    bool reader_crc_sequence_valid;
    bool reader_fifo_settle_valid;
    bool reader_idle_sequence_valid;
    bool reader_irq_grace_valid;
    bool reader_idle_armed;
    bool reader_transceive_active;
    uint64_t reader_fifo_flush_ms;
    uint64_t reader_transceive_ms;
} fake_transport_t;

static void *test_alloc(void *user, size_t size) {
    (void)user;
    return malloc(size);
}

static void test_free(void *user, void *pointer) {
    (void)user;
    free(pointer);
}

static const h2_pal_mem_vtable_t mem_vtable = {
    .alloc = test_alloc,
    .free = test_free,
};

static const h2_pal_mem_api_t mem_api = {
    .vtable = &mem_vtable,
};

static void queue_reader_response(fake_transport_t *fake,
                                  const uint8_t *data, size_t len) {
    memcpy(fake->rx_fifo, data, len);
    fake->rx_len = len;
    fake->rx_offset = 0u;
    fake->registers[REG_FIFO_LENGTH] = (uint8_t)len;
    fake->registers[REG_IRQ0] =
        fake->reader_rx_with_timer_irq ? 0x14u : 0x04u;
}

static h2_pal_result_t fake_reset(void *user) {
    fake_transport_t *fake = user;
    ++fake->reset_calls;
    fake->registers[REG_COMMAND] = 0u;
    fake->registers[REG_VERSION] = 0x60u;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_write_reg(void *user, uint8_t reg,
                                      uint8_t value) {
    fake_transport_t *fake = user;
    ++fake->write_calls;
    if (fake->fail_write_on_call != 0u &&
        fake->write_calls == fake->fail_write_on_call) {
        return H2_PAL_ERR_IO;
    }
    if (reg == REG_IRQ0 && value == 0x7fu) {
        fake->registers[REG_IRQ0] = 0u;
    } else {
        fake->registers[reg & 0x7fu] = value;
    }
    if (reg == REG_FIFO_CONTROL && (value & 0x10u) != 0u) {
        fake->tx_len = 0u;
        fake->reader_fifo_flush_ms = fake->now_ms;
    }
    if (reg == REG_COMMAND) {
        if (value == 0u) {
            fake->reader_idle_armed = true;
        }
        if (value == 0x07u) {
            bool crc_expected =
                (fake->tx_len == 7u && fake->tx_fifo[1] == 0x70u) ||
                (fake->tx_len == 2u && fake->tx_fifo[0] == 0x30u);
            bool crc_enabled =
                (fake->registers[REG_TX_CRC_CONTROL] & 0x01u) != 0u &&
                (fake->registers[REG_RX_CRC_CONTROL] & 0x01u) != 0u;
            if (crc_enabled != crc_expected) {
                fake->reader_crc_sequence_valid = false;
            }
            if (fake->now_ms - fake->reader_fifo_flush_ms < 2u) {
                fake->reader_fifo_settle_valid = false;
            }
            if (!fake->reader_idle_armed) {
                fake->reader_idle_sequence_valid = false;
            }
            fake->reader_idle_armed = false;
            fake->reader_transceive_active = true;
            fake->reader_transceive_ms = fake->now_ms;
        }
        if (fake->reader_controller_error && value == 0x07u) {
            fake->registers[REG_IRQ0] = 0x02u;
            fake->registers[REG_ERROR] = 0x01u;
        } else if (fake->reader_script_enabled && value == 0x07u) {
            if (fake->tx_len == 1u && fake->tx_fifo[0] == 0x26u) {
                uint8_t atqa0 = 0x44u;
                if (fake->reader_cl3_enabled) {
                    atqa0 = 0x84u;
                } else if (fake->reader_four_byte_uid ||
                           fake->reader_uid_length_mismatch) {
                    atqa0 = 0x04u;
                }
                const uint8_t response[] = {atqa0, 0x00u};
                queue_reader_response(
                    fake,
                    response,
                    fake->reader_short_atqa ? 1u : sizeof(response));
            } else if (fake->tx_len == 2u && fake->tx_fifo[0] == 0x93u) {
                if (fake->reader_four_byte_uid) {
                    uint8_t response[] = {0x04u, 0x01u, 0x02u,
                                          0x03u, 0x04u};
                    if (fake->reader_bad_bcc) {
                        response[4] ^= 0x01u;
                    }
                    queue_reader_response(fake, response, sizeof(response));
                } else {
                    uint8_t response[] = {0x88u, 0x04u, 0x01u,
                                          0x02u, 0x8fu};
                    if (fake->reader_bad_bcc) {
                        response[4] ^= 0x01u;
                    }
                    queue_reader_response(fake, response, sizeof(response));
                }
            } else if (fake->tx_len == 2u && fake->tx_fifo[0] == 0x95u) {
                if (fake->reader_cl3_enabled) {
                    const uint8_t response[] = {0x88u, 0x03u, 0x04u,
                                                0x05u, 0x8au};
                    queue_reader_response(fake, response, sizeof(response));
                } else {
                    const uint8_t response[] = {0x03u, 0x04u, 0x05u,
                                                0x06u, 0x04u};
                    queue_reader_response(fake, response, sizeof(response));
                }
            } else if (fake->tx_len == 2u && fake->tx_fifo[0] == 0x97u) {
                const uint8_t response[] = {0x06u, 0x07u, 0x08u, 0x09u,
                                            0x00u};
                queue_reader_response(fake, response, sizeof(response));
            } else if (fake->tx_len == 7u && fake->tx_fifo[0] == 0x93u) {
                uint8_t response[] = {
                    fake->reader_four_byte_uid ? 0x00u : 0x04u,
                };
                if (fake->reader_bad_sak_cascade) {
                    response[0] ^= 0x04u;
                }
                queue_reader_response(fake, response, sizeof(response));
            } else if (fake->tx_len == 7u && fake->tx_fifo[0] == 0x95u) {
                const uint8_t response[] = {
                    fake->reader_cl3_enabled ? 0x04u : 0x00u,
                };
                queue_reader_response(fake, response, sizeof(response));
            } else if (fake->tx_len == 7u && fake->tx_fifo[0] == 0x97u) {
                const uint8_t response[] = {0x00u};
                queue_reader_response(fake, response, sizeof(response));
            } else if (fake->tx_len == 2u && fake->tx_fifo[0] == 0x30u) {
                const uint8_t response[] = {
                    0x03u, 0x03u, 0xd1u, 0x01u, 0x00u, 0xfeu, 0u, 0u,
                    0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
                };
                queue_reader_response(fake, response, sizeof(response));
            }
        }
        fake->registers[REG_COMMAND] = 0u;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t fake_write_regs(void *user, uint8_t reg,
                                       const uint8_t *data, size_t len) {
    fake_transport_t *fake = user;
    if (data == NULL && len != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (reg != REG_FIFO_DATA) {
        for (size_t i = 0u; i < len; ++i) {
            h2_pal_result_t result = fake_write_reg(user, reg, data[i]);
            if (result != H2_PAL_OK) {
                return result;
            }
        }
        return H2_PAL_OK;
    }
    if (fake->tx_len + len > sizeof(fake->tx_fifo)) {
        return H2_PAL_ERR_NO_SPACE;
    }
    memcpy(&fake->tx_fifo[fake->tx_len], data, len);
    fake->tx_len += len;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_read_reg(void *user, uint8_t reg,
                                     uint8_t *out_value) {
    fake_transport_t *fake = user;
    if (out_value == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (reg == REG_IRQ0 && fake->reader_transceive_active) {
        if (fake->now_ms - fake->reader_transceive_ms < 12u) {
            fake->reader_irq_grace_valid = false;
        }
        fake->reader_transceive_active = false;
        if (fake->reader_irq_transport_timeout) {
            return H2_PAL_ERR_TIMEOUT;
        }
    }
    *out_value = fake->registers[reg & 0x7fu];
    return H2_PAL_OK;
}

static h2_pal_result_t fake_read_regs(void *user, uint8_t reg,
                                      uint8_t *data, size_t len) {
    fake_transport_t *fake = user;
    if ((data == NULL && len != 0u) || reg != REG_FIFO_DATA ||
        fake->rx_offset + len > fake->rx_len) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memcpy(data, &fake->rx_fifo[fake->rx_offset], len);
    fake->rx_offset += len;
    return H2_PAL_OK;
}

static uint64_t fake_now_ms(void *user) {
    return ((fake_transport_t *)user)->now_ms;
}

static h2_pal_result_t fake_sleep_ms(void *user, uint32_t delay_ms) {
    ((fake_transport_t *)user)->now_ms += delay_ms;
    return H2_PAL_OK;
}

static const h2_fm17660k_transport_vtable_t transport_vtable = {
    .reset = fake_reset,
    .write_reg = fake_write_reg,
    .write_regs = fake_write_regs,
    .read_reg = fake_read_reg,
    .read_regs = fake_read_regs,
    .now_ms = fake_now_ms,
    .sleep_ms = fake_sleep_ms,
};

static h2_fm17660k_t *create_driver_with_timeout(fake_transport_t *fake,
                                                  uint32_t timeout_ms) {
    h2_fm17660k_t *driver = NULL;
    fake->reader_crc_sequence_valid = true;
    fake->reader_fifo_settle_valid = true;
    fake->reader_idle_sequence_valid = true;
    fake->reader_irq_grace_valid = true;
    const h2_fm17660k_config_t config = {
        .periph_id = 42u,
        .mem = &mem_api,
        .transport = {.user = fake, .vtable = &transport_vtable},
        .operation_timeout_ms = timeout_ms,
    };
    assert(h2_fm17660k_init(&config, &driver) == H2_PAL_OK);
    return driver;
}

static h2_fm17660k_t *create_driver(fake_transport_t *fake) {
    return create_driver_with_timeout(fake, 25u);
}

static void test_managed_emulation_is_one_sync_call(void) {
    fake_transport_t fake = {0};
    h2_fm17660k_t *driver = create_driver(&fake);
    const h2_pal_nfc_card_emulation_api_t *api =
        h2_fm17660k_card_emulation_api(driver);
    h2_pal_nfc_card_emulation_capabilities_t capabilities;
    assert(h2_pal_nfc_card_emulation_get_capabilities(
               api, 41u, &capabilities) == H2_PAL_ERR_NOT_FOUND);
    assert(h2_pal_nfc_card_emulation_get_capabilities(
               api, 42u, &capabilities) == H2_PAL_OK);
    assert((capabilities.managed_profile_mask &
            H2_PAL_NFC_CARD_EMULATION_PROFILE_TYPE2_READ_ONLY) != 0u);
    assert(capabilities.raw.provider_owns_crc);

    const uint8_t uid[] = {0x04u, 1u, 2u, 3u, 4u, 5u, 6u};
    const uint8_t ndef[] = {0xd1u, 0x01u, 0x01u, 0x54u, 0x00u};
    const h2_pal_nfc_card_emulation_config_t config = {
        .periph_id = 42u,
        .technology = H2_PAL_NFC_CARD_EMULATION_TECHNOLOGY_ISO14443A,
        .mode = H2_PAL_NFC_CARD_EMULATION_MODE_MANAGED_PROFILE,
        .managed_profile =
            H2_PAL_NFC_CARD_EMULATION_PROFILE_TYPE2_READ_ONLY,
        .uid = uid,
        .uid_len = sizeof(uid),
        .content_revision = 7u,
        .managed_bytes = ndef,
        .managed_len = sizeof(ndef),
        .window_ms = 5u,
    };
    h2_pal_nfc_card_emulation_result_t result;
    size_t writes_before_invalid_calls = fake.write_calls;
    assert(api->vtable->emulate(api->user, NULL, &result) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(api->vtable->emulate(api->user, &config, NULL) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(fake.write_calls == writes_before_invalid_calls);
    assert(h2_pal_nfc_card_emulate(api, &config, &result) == H2_PAL_OK);
    assert(result.completion ==
           H2_PAL_NFC_CARD_EMULATION_COMPLETION_WINDOW_EXPIRED);
    assert(result.content_revision == 7u);
    assert(fake.now_ms >= 5u);
    h2_fm17660k_deinit(driver);
}

static void test_reader_path_remains_functional(void) {
    fake_transport_t fake = {.reader_script_enabled = true};
    h2_fm17660k_t *driver = create_driver(&fake);
    h2_pal_nfc_scan_t scan;
    assert(h2_pal_nfc_scan_nfc_reader(
               h2_fm17660k_reader_api(driver), 42u, &scan) == H2_PAL_OK);
    const uint8_t expected_uid[] = {0x04u, 0x01u, 0x02u, 0x03u,
                                    0x04u, 0x05u, 0x06u};
    assert(scan.uid_len == sizeof(expected_uid));
    assert(memcmp(scan.uid, expected_uid, sizeof(expected_uid)) == 0);
    assert(fake.reader_crc_sequence_valid);
    assert(fake.reader_fifo_settle_valid);
    assert(fake.reader_idle_sequence_valid);
    assert(fake.reader_irq_grace_valid);

    h2_pal_nfc_data_read_t data;
    assert(h2_pal_nfc_read_nfc_data(
               h2_fm17660k_reader_api(driver), 42u, expected_uid,
               sizeof(expected_uid), H2_PAL_NFC_DATA_NDEF, &mem_api,
               &data) == H2_PAL_OK);
    assert(data.len == 3u && data.bytes[0] == 0xd1u);
    assert(fake.reader_crc_sequence_valid);
    assert(fake.reader_irq_grace_valid);
    h2_pal_mem_free(&mem_api, data.bytes);
    h2_fm17660k_deinit(driver);
}

static void test_reader_timeout_reports_absent(void) {
    fake_transport_t fake = {0};
    h2_fm17660k_t *driver = create_driver(&fake);
    h2_pal_nfc_scan_t scan;
    assert(h2_pal_nfc_scan_nfc_reader(
               h2_fm17660k_reader_api(driver), 42u, &scan) == H2_PAL_OK);
    assert(scan.stage == H2_PAL_NFC_STAGE_ABSENT);
    assert(scan.result == H2_PAL_OK);
    assert(scan.uid_len == 0u);
    assert(fake.now_ms >= 25u);
    assert(fake.reader_crc_sequence_valid);
    assert(fake.reader_fifo_settle_valid);
    assert(fake.reader_idle_sequence_valid);
    assert(fake.reader_irq_grace_valid);
    h2_fm17660k_deinit(driver);
}

static void test_reader_timeout_bounds_receive_grace(void) {
    fake_transport_t fake = {0};
    h2_fm17660k_t *driver = create_driver_with_timeout(&fake, 1u);
    h2_pal_nfc_scan_t scan;
    assert(h2_pal_nfc_scan_nfc_reader(
               h2_fm17660k_reader_api(driver), 42u, &scan) == H2_PAL_OK);
    assert(scan.stage == H2_PAL_NFC_STAGE_ABSENT);
    assert(scan.result == H2_PAL_OK);
    assert(fake.now_ms - fake.reader_transceive_ms == 1u);
    h2_fm17660k_deinit(driver);
}

static void test_reader_prefers_rx_done_over_timer_irq(void) {
    fake_transport_t fake = {
        .reader_script_enabled = true,
        .reader_rx_with_timer_irq = true,
    };
    h2_fm17660k_t *driver = create_driver_with_timeout(&fake, 12u);
    h2_pal_nfc_scan_t scan;
    assert(h2_pal_nfc_scan_nfc_reader(
               h2_fm17660k_reader_api(driver), 42u, &scan) == H2_PAL_OK);
    assert(scan.stage == H2_PAL_NFC_STAGE_DISCOVERED);
    assert(scan.uid_len == 7u);
    h2_fm17660k_deinit(driver);
}

static void test_reader_transport_timeout_is_error(void) {
    fake_transport_t fake = {.reader_irq_transport_timeout = true};
    h2_fm17660k_t *driver = create_driver(&fake);
    h2_pal_nfc_scan_t scan;
    assert(h2_pal_nfc_scan_nfc_reader(
               h2_fm17660k_reader_api(driver), 42u, &scan) ==
           H2_PAL_ERR_TIMEOUT);
    assert(scan.stage == H2_PAL_NFC_STAGE_ERROR);
    assert(scan.result == H2_PAL_ERR_TIMEOUT);
    h2_fm17660k_deinit(driver);
}

static void assert_reader_scan_error(fake_transport_t *fake,
                                     h2_pal_result_t expected_result) {
    h2_fm17660k_t *driver = create_driver(fake);
    h2_pal_nfc_scan_t scan;
    assert(h2_pal_nfc_scan_nfc_reader(
               h2_fm17660k_reader_api(driver), 42u, &scan) ==
           expected_result);
    assert(scan.stage == H2_PAL_NFC_STAGE_ERROR);
    assert(scan.result == expected_result);
    assert(scan.uid_len == 0u);
    h2_fm17660k_deinit(driver);
}

static void test_reader_controller_error_is_io(void) {
    fake_transport_t fake = {.reader_controller_error = true};
    assert_reader_scan_error(&fake, H2_PAL_ERR_IO);
}

static void test_reader_rejects_malformed_activation(void) {
    fake_transport_t short_atqa = {
        .reader_script_enabled = true,
        .reader_short_atqa = true,
    };
    assert_reader_scan_error(&short_atqa, H2_PAL_ERR_FORMAT);

    fake_transport_t bad_bcc = {
        .reader_script_enabled = true,
        .reader_bad_bcc = true,
    };
    assert_reader_scan_error(&bad_bcc, H2_PAL_ERR_FORMAT);

    fake_transport_t bad_sak_cascade = {
        .reader_script_enabled = true,
        .reader_bad_sak_cascade = true,
    };
    assert_reader_scan_error(&bad_sak_cascade, H2_PAL_ERR_FORMAT);

    fake_transport_t uid_length_mismatch = {
        .reader_script_enabled = true,
        .reader_uid_length_mismatch = true,
    };
    assert_reader_scan_error(&uid_length_mismatch, H2_PAL_ERR_FORMAT);
}

static void test_reader_supports_ten_byte_uid(void) {
    fake_transport_t fake = {
        .reader_script_enabled = true,
        .reader_cl3_enabled = true,
    };
    h2_fm17660k_t *driver = create_driver(&fake);
    h2_pal_nfc_scan_t scan;
    assert(h2_pal_nfc_scan_nfc_reader(
               h2_fm17660k_reader_api(driver), 42u, &scan) == H2_PAL_OK);
    const uint8_t expected_uid[] = {
        0x04u, 0x01u, 0x02u, 0x03u, 0x04u,
        0x05u, 0x06u, 0x07u, 0x08u, 0x09u,
    };
    assert(scan.uid_len == sizeof(expected_uid));
    assert(memcmp(scan.uid, expected_uid, sizeof(expected_uid)) == 0);
    assert(fake.reader_crc_sequence_valid);
    h2_fm17660k_deinit(driver);
}

static void test_reader_supports_four_byte_uid(void) {
    fake_transport_t fake = {
        .reader_script_enabled = true,
        .reader_four_byte_uid = true,
    };
    h2_fm17660k_t *driver = create_driver(&fake);
    h2_pal_nfc_scan_t scan;
    assert(h2_pal_nfc_scan_nfc_reader(
               h2_fm17660k_reader_api(driver), 42u, &scan) == H2_PAL_OK);
    const uint8_t expected_uid[] = {0x04u, 0x01u, 0x02u, 0x03u};
    assert(scan.uid_len == sizeof(expected_uid));
    assert(memcmp(scan.uid, expected_uid, sizeof(expected_uid)) == 0);
    assert(fake.reader_crc_sequence_valid);
    assert(fake.reader_irq_grace_valid);
    h2_fm17660k_deinit(driver);
}

static void test_reader_read_requires_matching_discovery(void) {
    fake_transport_t absent = {0};
    h2_fm17660k_t *driver = create_driver(&absent);
    h2_pal_nfc_data_read_t data = {
        .bytes = (uint8_t *)(uintptr_t)1u,
        .len = 1u,
    };
    assert(h2_pal_nfc_read_nfc_data(
               h2_fm17660k_reader_api(driver), 42u, NULL, 0u,
               H2_PAL_NFC_DATA_NDEF, &mem_api, &data) ==
           H2_PAL_ERR_NOT_FOUND);
    assert(data.bytes == NULL);
    assert(data.len == 0u);
    h2_fm17660k_deinit(driver);

    fake_transport_t discovered = {.reader_script_enabled = true};
    driver = create_driver(&discovered);
    const uint8_t wrong_uid[] = {0x04u, 0x01u, 0x02u, 0x03u};
    data = (h2_pal_nfc_data_read_t){
        .bytes = (uint8_t *)(uintptr_t)1u,
        .len = 1u,
    };
    assert(h2_pal_nfc_read_nfc_data(
               h2_fm17660k_reader_api(driver), 42u, wrong_uid,
               sizeof(wrong_uid), H2_PAL_NFC_DATA_NDEF, &mem_api,
               &data) == H2_PAL_ERR_NOT_FOUND);
    assert(data.bytes == NULL);
    assert(data.len == 0u);
    h2_fm17660k_deinit(driver);
}

static void test_init_failure_and_recovery(void) {
    fake_transport_t fake = {.fail_write_on_call = 1u};
    h2_fm17660k_t *driver = (h2_fm17660k_t *)(uintptr_t)1u;
    const h2_fm17660k_config_t config = {
        .periph_id = 42u,
        .mem = &mem_api,
        .transport = {.user = &fake, .vtable = &transport_vtable},
        .operation_timeout_ms = 25u,
    };
    assert(h2_fm17660k_init(&config, &driver) == H2_PAL_ERR_IO);
    assert(driver == NULL);

    fake = (fake_transport_t){0};
    driver = create_driver(&fake);
    assert(h2_fm17660k_recover(driver) == H2_PAL_OK);
    assert(fake.reset_calls == 2u);
    h2_fm17660k_deinit(driver);
}

typedef struct fake_endpoint {
    uint8_t writes[16];
    size_t write_len;
    uint8_t reads[8];
    size_t read_len;
    size_t read_offset;
} fake_endpoint_t;

static h2_pal_result_t endpoint_write(void *user, const uint8_t *data,
                                      size_t len, uint32_t timeout_ms) {
    fake_endpoint_t *endpoint = user;
    assert(timeout_ms == 20u);
    assert(endpoint->write_len + len <= sizeof(endpoint->writes));
    memcpy(&endpoint->writes[endpoint->write_len], data, len);
    endpoint->write_len += len;
    return H2_PAL_OK;
}

static h2_pal_result_t endpoint_read(void *user, uint8_t *data, size_t len,
                                     uint32_t timeout_ms) {
    fake_endpoint_t *endpoint = user;
    assert(timeout_ms == 20u);
    if (endpoint->read_offset + len > endpoint->read_len) {
        return H2_PAL_ERR_IO;
    }
    memcpy(data, &endpoint->reads[endpoint->read_offset], len);
    endpoint->read_offset += len;
    return H2_PAL_OK;
}

static void test_byte_endpoint_encodes_uart_commands(void) {
    fake_endpoint_t endpoint = {
        .reads = {0x12u, 0x5au},
        .read_len = 2u,
    };
    fake_transport_t clock = {0};
    const h2_fm17660k_byte_transport_config_t config = {
        .endpoint = {
            .user = &endpoint,
            .write_exact = endpoint_write,
            .read_exact = endpoint_read,
        },
        .reset = fake_reset,
        .reset_user = &clock,
        .now_ms = fake_now_ms,
        .sleep_ms = fake_sleep_ms,
        .clock_user = &clock,
        .operation_timeout_ms = 20u,
    };
    h2_fm17660k_transport_t transport;
    assert(h2_fm17660k_transport_from_byte_endpoint(&config, &transport) ==
           H2_PAL_OK);
    assert(transport.vtable->write_reg(transport.user, 0x12u, 0x34u) ==
           H2_PAL_OK);
    uint8_t value = 0u;
    assert(transport.vtable->read_reg(transport.user, 0x22u, &value) ==
           H2_PAL_OK);
    const uint8_t expected[] = {0x12u, 0x34u, 0xa2u};
    assert(endpoint.write_len == sizeof(expected));
    assert(memcmp(endpoint.writes, expected, sizeof(expected)) == 0);
    assert(value == 0x5au);
}

int main(void) {
    test_managed_emulation_is_one_sync_call();
    test_reader_path_remains_functional();
    test_reader_timeout_reports_absent();
    test_reader_timeout_bounds_receive_grace();
    test_reader_prefers_rx_done_over_timer_irq();
    test_reader_transport_timeout_is_error();
    test_reader_controller_error_is_io();
    test_reader_rejects_malformed_activation();
    test_reader_supports_ten_byte_uid();
    test_reader_supports_four_byte_uid();
    test_reader_read_requires_matching_discovery();
    test_init_failure_and_recovery();
    test_byte_endpoint_encodes_uart_commands();
    return 0;
}
