#include "h2_h2loader_host_flash.h"
#include "h2_h2loader_host_factory.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define TEST_SECTOR_SIZE 4096u
#define TEST_BUNDLE_SIZE (H2_H2LOADER_HOST_FACTORY_HEADER_SIZE + 4u)

typedef struct bk_fixture {
    h2_pal_uart_io_stream_api_t uart;
    uint8_t response[TEST_SECTOR_SIZE + 32u];
    size_t response_len;
    size_t response_offset;
    uint8_t flash[TEST_SECTOR_SIZE];
    uint32_t baud;
    uint64_t now_ms;
    unsigned open_count;
    unsigned close_count;
    unsigned erase_count;
    unsigned write_count;
    unsigned read_count;
    unsigned reboot_count;
    int corrupt_readback;
} bk_fixture_t;

typedef struct bundle_fixture {
    uint8_t bytes[TEST_BUNDLE_SIZE];
} bundle_fixture_t;

static void write_le16(uint8_t *out, uint16_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8u);
}

static void write_le32(uint8_t *out, uint32_t value) {
    for (size_t i = 0u; i < 4u; ++i) {
        out[i] = (uint8_t)(value >> (i * 8u));
    }
}

static void write_le64(uint8_t *out, uint64_t value) {
    for (size_t i = 0u; i < 8u; ++i) {
        out[i] = (uint8_t)(value >> (i * 8u));
    }
}

static uint16_t read_le16(const uint8_t *data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8u);
}

static uint32_t read_le32(const uint8_t *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8u) |
        ((uint32_t)data[2] << 16u) | ((uint32_t)data[3] << 24u);
}

static void queue_common(
    bk_fixture_t *fixture,
    uint8_t command,
    const uint8_t *params,
    size_t param_len) {
    assert(param_len <= 250u);
    fixture->response[0] = 0x04u;
    fixture->response[1] = 0x0eu;
    fixture->response[2] = (uint8_t)(4u + param_len);
    fixture->response[3] = 0x01u;
    fixture->response[4] = 0xe0u;
    fixture->response[5] = 0xfcu;
    fixture->response[6] = command;
    if (param_len > 0u) {
        memcpy(&fixture->response[7], params, param_len);
    }
    fixture->response_len = 7u + param_len;
    fixture->response_offset = 0u;
}

static void queue_flash(
    bk_fixture_t *fixture,
    uint8_t command,
    uint8_t status,
    const uint8_t *params,
    size_t param_len) {
    assert(param_len + 11u <= sizeof(fixture->response));
    fixture->response[0] = 0x04u;
    fixture->response[1] = 0x0eu;
    fixture->response[2] = 0xffu;
    fixture->response[3] = 0x01u;
    fixture->response[4] = 0xe0u;
    fixture->response[5] = 0xfcu;
    fixture->response[6] = 0xf4u;
    write_le16(&fixture->response[7], (uint16_t)(param_len + 2u));
    fixture->response[9] = command;
    fixture->response[10] = status;
    if (param_len > 0u) {
        memcpy(&fixture->response[11], params, param_len);
    }
    fixture->response_len = 11u + param_len;
    fixture->response_offset = 0u;
}

static h2_pal_result_t fake_uart_configure(
    void *user,
    const h2_pal_uart_io_stream_config_t *config) {
    bk_fixture_t *fixture = user;
    assert(config->data_bits == 8u);
    assert(config->stop_bits == 1u);
    assert(config->parity == H2_PAL_UART_PARITY_NONE);
    assert(config->flow_control == H2_PAL_UART_FLOW_CONTROL_NONE);
    fixture->baud = config->baud_rate;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_uart_read(
    void *user,
    void *buffer,
    size_t len,
    size_t *out_read,
    uint32_t timeout_ms) {
    bk_fixture_t *fixture = user;
    *out_read = 0u;
    if (fixture->response_offset >= fixture->response_len) {
        fixture->now_ms += timeout_ms;
        return H2_PAL_ERR_TIMEOUT;
    }
    size_t remaining =
        fixture->response_len - fixture->response_offset;
    size_t take = remaining < len ? remaining : len;
    memcpy(
        buffer,
        &fixture->response[fixture->response_offset],
        take);
    fixture->response_offset += take;
    *out_read = take;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_uart_write(
    void *user,
    const void *buffer,
    size_t len,
    size_t *out_written,
    uint32_t timeout_ms) {
    bk_fixture_t *fixture = user;
    const uint8_t *data = buffer;
    (void)timeout_ms;
    *out_written = 0u;
    if (len < 5u ||
        data[0] != 0x01u ||
        data[1] != 0xe0u ||
        data[2] != 0xfcu) {
        return H2_PAL_ERR_FORMAT;
    }
    if (data[3] != 0xffu) {
        size_t param_len = data[3] - 1u;
        if (len != param_len + 5u) {
            return H2_PAL_ERR_FORMAT;
        }
        if (data[4] == 0x00u) {
            const uint8_t ok = 0x00u;
            queue_common(fixture, 0x01u, &ok, 1u);
        } else if (data[4] == 0xaau) {
            queue_common(fixture, 0xaau, &data[5], param_len);
        } else if (data[4] == 0x0fu) {
            queue_common(fixture, 0x0fu, &data[5], param_len);
        } else if (data[4] == 0x0eu) {
            fixture->reboot_count++;
            fixture->response_len = 0u;
            fixture->response_offset = 0u;
        } else {
            return H2_PAL_ERR_UNSUPPORTED;
        }
        *out_written = len;
        return H2_PAL_OK;
    }
    if (len < 8u || data[4] != 0xf4u ||
        read_le16(&data[5]) + 7u != len) {
        return H2_PAL_ERR_FORMAT;
    }
    uint8_t command = data[7];
    const uint8_t *params = &data[8];
    size_t param_len = len - 8u;
    if (command == 0x0fu) {
        assert(param_len == 5u);
        assert(params[0] == 0xd8u);
        assert((read_le32(&params[1]) % (64u * 1024u)) == 0u);
        if (read_le32(&params[1]) == 0u) {
            memset(fixture->flash, 0xff, sizeof(fixture->flash));
        }
        fixture->erase_count++;
        queue_flash(fixture, command, 0u, params, 5u);
    } else if (command == 0x07u) {
        assert(param_len == TEST_SECTOR_SIZE + 4u);
        assert(read_le32(params) == 0u);
        memcpy(fixture->flash, &params[4], TEST_SECTOR_SIZE);
        fixture->write_count++;
        queue_flash(fixture, command, 0u, params, 4u);
    } else if (command == 0x09u) {
        assert(param_len == 4u);
        assert(read_le32(params) == 0u);
        uint8_t response[TEST_SECTOR_SIZE + 4u];
        memcpy(response, params, 4u);
        memcpy(&response[4], fixture->flash, TEST_SECTOR_SIZE);
        if (fixture->corrupt_readback) {
            response[4] ^= 1u;
        }
        fixture->read_count++;
        queue_flash(
            fixture,
            command,
            0u,
            response,
            sizeof(response));
    } else {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    *out_written = len;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_uart_flush(void *user) {
    (void)user;
    return H2_PAL_OK;
}

static const h2_pal_uart_io_stream_vtable_t fake_uart_vtable = {
    .configure = fake_uart_configure,
    .read = fake_uart_read,
    .write = fake_uart_write,
    .flush = fake_uart_flush,
};

static h2_pal_result_t fake_serial_open(
    void *user,
    const char *port_id,
    const h2_pal_uart_io_stream_config_t *config,
    h2_pal_serial_host_session_t **out_session) {
    bk_fixture_t *fixture = user;
    assert(strcmp(port_id, "bk-port") == 0);
    assert(config->baud_rate == 115200u);
    fixture->baud = config->baud_rate;
    fixture->open_count++;
    *out_session = (h2_pal_serial_host_session_t *)fixture;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_serial_stream(
    void *user,
    h2_pal_serial_host_session_t *session,
    const h2_pal_uart_io_stream_api_t **out_stream) {
    bk_fixture_t *fixture = user;
    assert(session == (h2_pal_serial_host_session_t *)fixture);
    *out_stream = &fixture->uart;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_serial_close(
    void *user,
    h2_pal_serial_host_session_t **inout_session) {
    bk_fixture_t *fixture = user;
    assert(*inout_session == (h2_pal_serial_host_session_t *)fixture);
    fixture->close_count++;
    *inout_session = NULL;
    return H2_PAL_OK;
}

static const h2_pal_serial_host_vtable_t fake_serial_vtable = {
    .open = fake_serial_open,
    .session_stream = fake_serial_stream,
    .close = fake_serial_close,
};

static h2_pal_result_t fake_time_now(void *user, uint64_t *out_ms) {
    bk_fixture_t *fixture = user;
    *out_ms = fixture->now_ms;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_time_sleep(void *user, uint32_t ms) {
    bk_fixture_t *fixture = user;
    fixture->now_ms += ms;
    return H2_PAL_OK;
}

static const h2_pal_time_vtable_t fake_time_vtable = {
    .get_monotonic_ms = fake_time_now,
    .sleep_ms = fake_time_sleep,
};

static void *test_alloc(void *user, size_t len) {
    (void)user;
    return malloc(len);
}

static void *test_realloc(void *user, void *ptr, size_t len) {
    (void)user;
    return realloc(ptr, len);
}

static void test_free(void *user, void *ptr) {
    (void)user;
    free(ptr);
}

static const h2_pal_mem_vtable_t test_mem_vtable = {
    .alloc = test_alloc,
    .realloc = test_realloc,
    .free = test_free,
};

static const h2_pal_mem_api_t test_mem = {
    .user = NULL,
    .vtable = &test_mem_vtable,
};

static h2_pal_result_t bundle_read(
    void *user,
    uint64_t offset,
    uint8_t *out,
    size_t out_size,
    size_t *out_read) {
    bundle_fixture_t *fixture = user;
    *out_read = 0u;
    if (offset > sizeof(fixture->bytes)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    size_t remaining = sizeof(fixture->bytes) - (size_t)offset;
    size_t take = remaining < out_size ? remaining : out_size;
    memcpy(out, &fixture->bytes[offset], take);
    *out_read = take;
    return H2_PAL_OK;
}

static void build_bundle(bundle_fixture_t *bundle) {
    static const uint8_t digest[32] = {
        0x88u, 0xd4u, 0x26u, 0x6fu, 0xd4u, 0xe6u, 0x33u, 0x8du,
        0x13u, 0xb8u, 0x45u, 0xfcu, 0xf2u, 0x89u, 0x57u, 0x9du,
        0x20u, 0x9cu, 0x89u, 0x78u, 0x23u, 0xb9u, 0x21u, 0x7du,
        0xa3u, 0xe1u, 0x61u, 0x93u, 0x6fu, 0x03u, 0x15u, 0x89u,
    };
    memset(bundle, 0, sizeof(*bundle));
    memcpy(bundle->bytes, "H2FB", 4u);
    write_le16(&bundle->bytes[4], 1u);
    write_le16(
        &bundle->bytes[6],
        H2_H2LOADER_HOST_FACTORY_DRIVER_BK7258_ROM);
    write_le32(&bundle->bytes[8], 1u);
    write_le32(&bundle->bytes[12], 500000u);
    write_le32(&bundle->bytes[16], 1u);
    strcpy((char *)&bundle->bytes[20], "bk7258_v3_202405");
    const size_t target_offset =
        20u + H2_H2LOADER_HOST_IDENTITY_MAX_LEN;
    const size_t record_offset =
        20u + 2u * H2_H2LOADER_HOST_IDENTITY_MAX_LEN;
    strcpy((char *)&bundle->bytes[target_offset], "bk7258");
    write_le32(&bundle->bytes[record_offset], 0u);
    write_le64(
        &bundle->bytes[record_offset + 4u],
        H2_H2LOADER_HOST_FACTORY_HEADER_SIZE);
    write_le64(&bundle->bytes[record_offset + 12u], 4u);
    memcpy(
        &bundle->bytes[record_offset + 20u],
        digest,
        sizeof(digest));
    strcpy(
        (char *)&bundle->bytes[record_offset + 52u],
        "all-app.bin");
    memcpy(
        &bundle->bytes[H2_H2LOADER_HOST_FACTORY_HEADER_SIZE],
        "abcd",
        4u);
}

static int never_cancelled(void *user) {
    (void)user;
    return 0;
}

int main(void) {
    bk_fixture_t fixture;
    memset(&fixture, 0, sizeof(fixture));
    memset(fixture.flash, 0x00, sizeof(fixture.flash));
    fixture.uart.user = &fixture;
    fixture.uart.vtable = &fake_uart_vtable;
    const h2_pal_serial_host_api_t serial = {
        .user = &fixture,
        .vtable = &fake_serial_vtable,
    };
    const h2_pal_time_api_t time = {
        .user = &fixture,
        .vtable = &fake_time_vtable,
    };
    bundle_fixture_t bundle;
    build_bundle(&bundle);
    h2_h2loader_host_catalog_entry_t asset;
    memset(&asset, 0, sizeof(asset));
    strcpy(asset.board, "bk7258_v3_202405");
    strcpy(asset.target, "bk7258");
    strcpy(asset.image, "h2loader");
    asset.role = H2_H2LOADER_HOST_ASSET_ROLE_LOADER;
    asset.operation = H2_H2LOADER_HOST_ASSET_OPERATION_RECOVERY;
    asset.bytes = sizeof(bundle.bytes);
    h2_h2loader_host_factory_manifest_t parsed_manifest;
    h2_pal_result_t parse_rc = h2_h2loader_host_factory_open(
        &asset, bundle_read, &bundle, &parsed_manifest);
    assert(parse_rc == H2_PAL_OK);
    const h2_h2loader_host_bk7258_flash_config_t config = {
        .serial = &serial,
        .time = &time,
        .allocator = &test_mem,
        .port_id = "bk-port",
        .expected_target = "bk7258",
        .connect_attempts = 2u,
    };
    h2_h2loader_host_flash_driver_t driver;
    assert(h2_h2loader_host_bk7258_flash_open(
               &config, &driver) == H2_PAL_OK);
    const h2_h2loader_host_recovery_authorization_t authorization = {
        .transport = H2_H2LOADER_HOST_TRANSPORT_SERIAL,
        .reason = H2_H2LOADER_HOST_RECOVERY_BLANK_FIXTURE,
        .probe_result = H2_PAL_ERR_TIMEOUT,
        .probe_completed_ms = 1u,
        .expires_ms = 1000u,
        .probe_attempts = 2u,
        .identity_confirmed = 1u,
        .destructive_confirmed = 1u,
    };
    h2_pal_result_t rc = h2_h2loader_host_recovery_run(
               &authorization,
               &asset,
               2u,
               &driver,
               bundle_read,
               &bundle,
               never_cancelled,
               NULL,
               NULL,
               NULL);
    assert(rc == H2_PAL_OK);
    assert(fixture.open_count == 1u);
    assert(fixture.close_count == 1u);
    assert(fixture.baud == 500000u);
    assert(fixture.erase_count == 128u);
    assert(fixture.write_count == 1u);
    assert(fixture.read_count == 1u);
    assert(fixture.reboot_count == 1u);
    assert(memcmp(fixture.flash, "abcd", 4u) == 0);
    for (size_t i = 4u; i < sizeof(fixture.flash); ++i) {
        assert(fixture.flash[i] == 0xffu);
    }

    fixture.corrupt_readback = 1;
    fixture.response_len = 0u;
    fixture.response_offset = 0u;
    assert(h2_h2loader_host_bk7258_flash_open(
               &config, &driver) == H2_PAL_OK);
    rc = h2_h2loader_host_recovery_run(
        &authorization,
        &asset,
        2u,
        &driver,
        bundle_read,
        &bundle,
        never_cancelled,
        NULL,
        NULL,
        NULL);
    assert(rc == H2_PAL_ERR_FORMAT);
    assert(fixture.open_count == 2u);
    assert(fixture.close_count == 2u);
    assert(fixture.reboot_count == 1u);
    return 0;
}
