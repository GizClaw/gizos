#include "h2_h2loader_host_flash.h"

#include "h2_h2loader_host_factory.h"
#include "h2_h2loader_host_internal.h"

#include <string.h>

/*
 * BK HCI framing follows Beken's Apache-2.0 boot protocol contract. This
 * implementation is host-side PAL code and does not link Beken SDK code.
 */
#define H2_BK7258_INITIAL_BAUD 115200u
#define H2_BK7258_MAX_BAUD 3000000u
#define H2_BK7258_SECTOR_SIZE 4096u
#define H2_BK7258_FLASH_SIZE (8u * 1024u * 1024u)
#define H2_BK7258_ERASE_BLOCK_SIZE (64u * 1024u)
#define H2_BK7258_CONNECT_ATTEMPTS 20u
#define H2_BK7258_IO_TIMEOUT_MS 2000u
#define H2_BK7258_ERASE_TIMEOUT_MS 5000u
#define H2_BK7258_WRITE_TIMEOUT_MS 10000u
#define H2_BK7258_READ_TIMEOUT_MS 10000u
#define H2_BK7258_CONNECT_DELAY_MS 50u
#define H2_BK7258_BAUD_DELAY_MS 50u
#define H2_BK7258_FRAME_CAPACITY (H2_BK7258_SECTOR_SIZE + 32u)

#define H2_BK7258_COMMAND_LINK_CHECK 0x00u
#define H2_BK7258_RESPONSE_LINK_CHECK 0x01u
#define H2_BK7258_COMMAND_REBOOT 0x0eu
#define H2_BK7258_COMMAND_SET_BAUD 0x0fu
#define H2_BK7258_COMMAND_STAY_ROM 0xaau
#define H2_BK7258_COMMAND_STARTUP 0xfeu
#define H2_BK7258_FLASH_SECTOR_WRITE 0x07u
#define H2_BK7258_FLASH_SECTOR_READ 0x09u
#define H2_BK7258_FLASH_SIZE_ERASE 0x0fu
#define H2_BK7258_FLASH_ERASE_64K 0xd8u

typedef struct h2_bk7258_flash_context {
    const h2_pal_serial_host_api_t *serial;
    const h2_pal_time_api_t *time;
    const h2_pal_mem_api_t *allocator;
    h2_pal_serial_host_session_t *session;
    const h2_pal_uart_io_stream_api_t *uart;
    uint8_t connect_attempts;
    uint32_t active_baud;
    h2_h2loader_host_factory_manifest_t manifest;
    h2_h2loader_host_payload_read_fn read_payload;
    void *payload_user;
    uint8_t frame[H2_BK7258_FRAME_CAPACITY];
} h2_bk7258_flash_context_t;

static void write_le16(uint8_t *out, uint16_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8u);
}

static void write_le32(uint8_t *out, uint32_t value) {
    for (size_t i = 0u; i < 4u; ++i) {
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

static h2_pal_result_t deadline_after(
    h2_bk7258_flash_context_t *context,
    uint32_t timeout_ms,
    uint64_t *out_deadline_ms) {
    uint64_t now_ms = 0u;
    h2_pal_result_t rc = h2_pal_time_get_monotonic_ms(
        context->time, &now_ms);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    *out_deadline_ms = UINT64_MAX - now_ms < timeout_ms
        ? UINT64_MAX
        : now_ms + timeout_ms;
    return H2_PAL_OK;
}

static h2_pal_result_t remaining_timeout(
    h2_bk7258_flash_context_t *context,
    uint64_t deadline_ms,
    uint32_t *out_timeout_ms) {
    uint64_t now_ms = 0u;
    h2_pal_result_t rc = h2_pal_time_get_monotonic_ms(
        context->time, &now_ms);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (now_ms >= deadline_ms) {
        *out_timeout_ms = 0u;
        return H2_PAL_ERR_TIMEOUT;
    }
    uint64_t remaining = deadline_ms - now_ms;
    *out_timeout_ms = remaining > UINT32_MAX
        ? UINT32_MAX
        : (uint32_t)remaining;
    return H2_PAL_OK;
}

static h2_pal_result_t write_all(
    h2_bk7258_flash_context_t *context,
    const uint8_t *data,
    size_t len,
    uint32_t timeout_ms) {
    size_t offset = 0u;
    uint64_t deadline_ms = 0u;
    h2_pal_result_t rc = deadline_after(
        context, timeout_ms, &deadline_ms);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    while (offset < len) {
        uint32_t remaining_ms = 0u;
        rc = remaining_timeout(
            context, deadline_ms, &remaining_ms);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        size_t written = 0u;
        rc = h2_pal_uart_io_stream_write(
            context->uart,
            &data[offset],
            len - offset,
            &written,
            remaining_ms);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        if (written == 0u || written > len - offset) {
            return H2_PAL_ERR_IO;
        }
        offset += written;
    }
    return h2_pal_uart_io_stream_flush(context->uart);
}

static h2_pal_result_t read_exact_until(
    h2_bk7258_flash_context_t *context,
    uint8_t *out,
    size_t len,
    uint64_t deadline_ms) {
    size_t offset = 0u;
    while (offset < len) {
        uint32_t remaining_ms = 0u;
        h2_pal_result_t rc = remaining_timeout(
            context, deadline_ms, &remaining_ms);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        size_t read = 0u;
        rc = h2_pal_uart_io_stream_read(
            context->uart,
            &out[offset],
            len - offset,
            &read,
            remaining_ms);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        if (read == 0u || read > len - offset) {
            return H2_PAL_ERR_TIMEOUT;
        }
        offset += read;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t read_frame(
    h2_bk7258_flash_context_t *context,
    size_t *out_len,
    uint32_t timeout_ms) {
    size_t sync = 0u;
    uint64_t deadline_ms = 0u;
    *out_len = 0u;
    h2_pal_result_t rc = deadline_after(
        context, timeout_ms, &deadline_ms);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    while (sync < 2u) {
        uint8_t byte = 0u;
        rc = read_exact_until(
            context, &byte, 1u, deadline_ms);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        if ((sync == 0u && byte == 0x04u) ||
            (sync == 1u && byte == 0x0eu)) {
            context->frame[sync++] = byte;
        } else {
            sync = byte == 0x04u ? 1u : 0u;
            if (sync == 1u) {
                context->frame[0] = byte;
            }
        }
    }
    rc = read_exact_until(
        context, &context->frame[2], 1u, deadline_ms);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    size_t total;
    if (context->frame[2] != 0xffu) {
        total = 3u + context->frame[2];
        if (total < 7u || total > sizeof(context->frame)) {
            return H2_PAL_ERR_FORMAT;
        }
        rc = read_exact_until(
            context,
            &context->frame[3],
            total - 3u,
            deadline_ms);
    } else {
        rc = read_exact_until(
            context, &context->frame[3], 6u, deadline_ms);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        size_t payload_len = read_le16(&context->frame[7]);
        total = 9u + payload_len;
        if (total < 11u || total > sizeof(context->frame)) {
            return H2_PAL_ERR_FORMAT;
        }
        rc = read_exact_until(
            context,
            &context->frame[9],
            total - 9u,
            deadline_ms);
    }
    if (rc != H2_PAL_OK) {
        return rc;
    }
    *out_len = total;
    return H2_PAL_OK;
}

static h2_pal_result_t common_command(
    h2_bk7258_flash_context_t *context,
    uint8_t command,
    const uint8_t *params,
    size_t param_len,
    uint8_t expected_response,
    uint32_t timeout_ms) {
    if (param_len > 250u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    uint8_t frame[256];
    frame[0] = 0x01u;
    frame[1] = 0xe0u;
    frame[2] = 0xfcu;
    frame[3] = (uint8_t)(param_len + 1u);
    frame[4] = command;
    if (param_len > 0u) {
        memcpy(&frame[5], params, param_len);
    }
    h2_pal_result_t rc = write_all(
        context, frame, 5u + param_len, timeout_ms);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    size_t response_len = 0u;
    rc = read_frame(context, &response_len, timeout_ms);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (response_len < 7u ||
        context->frame[2] == 0xffu ||
        context->frame[3] != 0x01u ||
        context->frame[4] != 0xe0u ||
        context->frame[5] != 0xfcu ||
        context->frame[6] != expected_response) {
        return H2_PAL_ERR_FORMAT;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t send_flash_command(
    h2_bk7258_flash_context_t *context,
    uint8_t command,
    const uint8_t *params,
    size_t param_len,
    uint32_t timeout_ms,
    size_t *out_response_len) {
    if (param_len > UINT16_MAX - 1u ||
        param_len + 8u > sizeof(context->frame)) {
        return H2_PAL_ERR_NO_SPACE;
    }
    uint16_t command_len = (uint16_t)(param_len + 1u);
    context->frame[0] = 0x01u;
    context->frame[1] = 0xe0u;
    context->frame[2] = 0xfcu;
    context->frame[3] = 0xffu;
    context->frame[4] = 0xf4u;
    write_le16(&context->frame[5], command_len);
    context->frame[7] = command;
    if (param_len > 0u) {
        memcpy(&context->frame[8], params, param_len);
    }
    h2_pal_result_t rc = write_all(
        context,
        context->frame,
        param_len + 8u,
        timeout_ms);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = read_frame(context, out_response_len, timeout_ms);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (*out_response_len < 11u ||
        context->frame[2] != 0xffu ||
        context->frame[3] != 0x01u ||
        context->frame[4] != 0xe0u ||
        context->frame[5] != 0xfcu ||
        context->frame[6] != 0xf4u ||
        context->frame[9] != command) {
        return H2_PAL_ERR_FORMAT;
    }
    return context->frame[10] == 0u
        ? H2_PAL_OK
        : H2_PAL_ERR_IO;
}

static h2_pal_result_t connect_rom(
    h2_bk7258_flash_context_t *context) {
    for (uint8_t attempt = 0u;
         attempt < context->connect_attempts;
         ++attempt) {
        uint8_t command[] = {
            0x01u,
            0xe0u,
            0xfcu,
            0x01u,
            H2_BK7258_COMMAND_LINK_CHECK,
        };
        h2_pal_result_t rc = write_all(
            context,
            command,
            sizeof(command),
            H2_BK7258_IO_TIMEOUT_MS);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        size_t response_len = 0u;
        rc = read_frame(
            context,
            &response_len,
            H2_BK7258_CONNECT_DELAY_MS);
        if (rc == H2_PAL_OK && response_len >= 7u &&
            context->frame[2] != 0xffu &&
            context->frame[3] == 0x01u &&
            context->frame[4] == 0xe0u &&
            context->frame[5] == 0xfcu) {
            if (context->frame[6] ==
                H2_BK7258_RESPONSE_LINK_CHECK) {
                return H2_PAL_OK;
            }
            if (context->frame[6] ==
                H2_BK7258_COMMAND_STARTUP) {
                rc = common_command(
                    context,
                    H2_BK7258_COMMAND_LINK_CHECK,
                    NULL,
                    0u,
                    H2_BK7258_RESPONSE_LINK_CHECK,
                    H2_BK7258_IO_TIMEOUT_MS);
                if (rc == H2_PAL_OK) {
                    return rc;
                }
            }
        } else if (rc != H2_PAL_ERR_TIMEOUT &&
                   rc != H2_PAL_ERR_WOULD_BLOCK) {
            return rc;
        }
        (void)h2_pal_time_sleep_ms(
            context->time, H2_BK7258_CONNECT_DELAY_MS);
    }
    return H2_PAL_ERR_TIMEOUT;
}

static h2_pal_result_t change_baud(
    h2_bk7258_flash_context_t *context,
    uint32_t baud) {
    if (baud == context->active_baud) {
        return H2_PAL_OK;
    }
    uint8_t params[5];
    write_le32(params, baud);
    params[4] = H2_BK7258_BAUD_DELAY_MS;
    uint8_t frame[10] = {
        0x01u,
        0xe0u,
        0xfcu,
        0x06u,
        H2_BK7258_COMMAND_SET_BAUD,
    };
    memcpy(&frame[5], params, sizeof(params));
    h2_pal_result_t rc = write_all(
        context, frame, sizeof(frame), H2_BK7258_IO_TIMEOUT_MS);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    const h2_pal_uart_io_stream_config_t config = {
        .baud_rate = baud,
        .data_bits = 8u,
        .stop_bits = 1u,
        .parity = H2_PAL_UART_PARITY_NONE,
        .flow_control = H2_PAL_UART_FLOW_CONTROL_NONE,
        .rx_buffer_size = 64u * 1024u,
        .tx_buffer_size = 8192u,
    };
    rc = h2_pal_uart_io_stream_configure(context->uart, &config);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    context->active_baud = baud;
    size_t response_len = 0u;
    rc = read_frame(
        context, &response_len, H2_BK7258_IO_TIMEOUT_MS);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (response_len != 12u ||
        context->frame[2] == 0xffu ||
        context->frame[6] != H2_BK7258_COMMAND_SET_BAUD ||
        memcmp(&context->frame[7], params, sizeof(params)) != 0) {
        return H2_PAL_ERR_FORMAT;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t erase_block(
    h2_bk7258_flash_context_t *context,
    uint32_t address) {
    uint8_t params[5];
    size_t response_len = 0u;
    params[0] = H2_BK7258_FLASH_ERASE_64K;
    write_le32(&params[1], address);
    h2_pal_result_t rc = send_flash_command(
        context,
        H2_BK7258_FLASH_SIZE_ERASE,
        params,
        sizeof(params),
        H2_BK7258_ERASE_TIMEOUT_MS,
        &response_len);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return response_len == 16u &&
            context->frame[11] == H2_BK7258_FLASH_ERASE_64K &&
            read_le32(&context->frame[12]) == address
        ? H2_PAL_OK
        : H2_PAL_ERR_FORMAT;
}

static h2_pal_result_t write_sector(
    h2_bk7258_flash_context_t *context,
    uint32_t address,
    const uint8_t *data) {
    uint8_t params[H2_BK7258_SECTOR_SIZE + 4u];
    size_t response_len = 0u;
    write_le32(params, address);
    memcpy(&params[4], data, H2_BK7258_SECTOR_SIZE);
    h2_pal_result_t rc = send_flash_command(
        context,
        H2_BK7258_FLASH_SECTOR_WRITE,
        params,
        sizeof(params),
        H2_BK7258_WRITE_TIMEOUT_MS,
        &response_len);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return response_len == 15u &&
            read_le32(&context->frame[11]) == address
        ? H2_PAL_OK
        : H2_PAL_ERR_FORMAT;
}

static h2_pal_result_t read_sector(
    h2_bk7258_flash_context_t *context,
    uint32_t address,
    const uint8_t **out_data) {
    uint8_t params[4];
    size_t response_len = 0u;
    write_le32(params, address);
    h2_pal_result_t rc = send_flash_command(
        context,
        H2_BK7258_FLASH_SECTOR_READ,
        params,
        sizeof(params),
        H2_BK7258_READ_TIMEOUT_MS,
        &response_len);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (response_len != H2_BK7258_SECTOR_SIZE + 15u ||
        read_le32(&context->frame[11]) != address) {
        return H2_PAL_ERR_FORMAT;
    }
    *out_data = &context->frame[15];
    return H2_PAL_OK;
}

static h2_pal_result_t bk7258_prepare(
    void *user,
    const h2_h2loader_host_catalog_entry_t *asset,
    h2_h2loader_host_payload_read_fn read_payload,
    void *payload_user) {
    h2_bk7258_flash_context_t *context = user;
    if (context == NULL || asset == NULL || read_payload == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t rc = h2_h2loader_host_factory_open(
        asset, read_payload, payload_user, &context->manifest);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (context->manifest.driver !=
            H2_H2LOADER_HOST_FACTORY_DRIVER_BK7258_ROM ||
        strcmp(context->manifest.target, "bk7258") != 0 ||
        context->manifest.baud < H2_BK7258_INITIAL_BAUD ||
        context->manifest.baud > H2_BK7258_MAX_BAUD) {
        memset(&context->manifest, 0, sizeof(context->manifest));
        return H2_PAL_ERR_INVALID_STATE;
    }
    for (size_t i = 0u; i < context->manifest.file_count; ++i) {
        const h2_h2loader_host_factory_file_t *file =
            &context->manifest.files[i];
        uint64_t rounded_end =
            (uint64_t)file->flash_offset +
            ((file->bytes + H2_BK7258_SECTOR_SIZE - 1u) /
             H2_BK7258_SECTOR_SIZE) *
                H2_BK7258_SECTOR_SIZE;
        if ((file->flash_offset % H2_BK7258_SECTOR_SIZE) != 0u ||
            rounded_end > H2_BK7258_FLASH_SIZE) {
            memset(&context->manifest, 0, sizeof(context->manifest));
            return H2_PAL_ERR_FORMAT;
        }
        for (size_t j = 0u; j < i; ++j) {
            const h2_h2loader_host_factory_file_t *previous =
                &context->manifest.files[j];
            uint64_t previous_end =
                (uint64_t)previous->flash_offset +
                ((previous->bytes + H2_BK7258_SECTOR_SIZE - 1u) /
                 H2_BK7258_SECTOR_SIZE) *
                    H2_BK7258_SECTOR_SIZE;
            if ((uint64_t)file->flash_offset < previous_end &&
                (uint64_t)previous->flash_offset < rounded_end) {
                memset(&context->manifest, 0, sizeof(context->manifest));
                return H2_PAL_ERR_FORMAT;
            }
        }
    }
    context->read_payload = read_payload;
    context->payload_user = payload_user;
    return H2_PAL_OK;
}

static h2_pal_result_t bk7258_erase(
    void *user,
    h2_h2loader_host_cancelled_fn is_cancelled,
    void *cancel_user) {
    h2_bk7258_flash_context_t *context = user;
    if (context == NULL || context->read_payload == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    h2_pal_result_t rc = connect_rom(context);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    const uint8_t stay_param = 0x55u;
    rc = common_command(
        context,
        H2_BK7258_COMMAND_STAY_ROM,
        &stay_param,
        1u,
        H2_BK7258_COMMAND_STAY_ROM,
        H2_BK7258_IO_TIMEOUT_MS);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = change_baud(context, context->manifest.baud);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    for (uint32_t address = 0u;
         address < H2_BK7258_FLASH_SIZE;
         address += H2_BK7258_ERASE_BLOCK_SIZE) {
        if (is_cancelled != NULL && is_cancelled(cancel_user)) {
            return H2_PAL_EXIT;
        }
        rc = erase_block(context, address);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    return H2_PAL_OK;
}

static h2_pal_result_t bk7258_write(
    void *user,
    const h2_h2loader_host_catalog_entry_t *asset,
    h2_h2loader_host_payload_read_fn read_payload,
    void *payload_user,
    h2_h2loader_host_cancelled_fn is_cancelled,
    void *cancel_user,
    h2_h2loader_host_progress_fn on_progress,
    void *progress_user) {
    h2_bk7258_flash_context_t *context = user;
    uint64_t total = 0u;
    uint64_t written_total = 0u;
    uint8_t sector[H2_BK7258_SECTOR_SIZE];
    if (context == NULL || asset == NULL ||
        context->read_payload != read_payload ||
        context->payload_user != payload_user ||
        context->manifest.driver !=
            H2_H2LOADER_HOST_FACTORY_DRIVER_BK7258_ROM ||
        strcmp(context->manifest.board, asset->board) != 0 ||
        strcmp(context->manifest.target, asset->target) != 0) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    for (size_t i = 0u; i < context->manifest.file_count; ++i) {
        total += context->manifest.files[i].bytes;
    }
    for (size_t i = 0u; i < context->manifest.file_count; ++i) {
        const h2_h2loader_host_factory_file_t *file =
            &context->manifest.files[i];
        for (uint64_t offset = 0u; offset < file->bytes;
             offset += H2_BK7258_SECTOR_SIZE) {
            if (is_cancelled != NULL && is_cancelled(cancel_user)) {
                return H2_PAL_EXIT;
            }
            memset(sector, 0xff, sizeof(sector));
            size_t request = file->bytes - offset > sizeof(sector)
                ? sizeof(sector)
                : (size_t)(file->bytes - offset);
            size_t read = 0u;
            h2_pal_result_t rc =
                h2_h2loader_host_factory_read_member(
                    &context->manifest,
                    i,
                    read_payload,
                    payload_user,
                    offset,
                    sector,
                    request,
                    &read);
            if (rc != H2_PAL_OK || read != request) {
                return rc == H2_PAL_OK
                    ? H2_PAL_ERR_TRUNCATED
                    : rc;
            }
            rc = write_sector(
                context,
                file->flash_offset + (uint32_t)offset,
                sector);
            if (rc != H2_PAL_OK) {
                return rc;
            }
            written_total += request;
            if (on_progress != NULL) {
                on_progress(progress_user, written_total, total);
            }
        }
    }
    return H2_PAL_OK;
}

static h2_pal_result_t bk7258_verify(
    void *user,
    h2_h2loader_host_cancelled_fn is_cancelled,
    void *cancel_user) {
    h2_bk7258_flash_context_t *context = user;
    if (context == NULL || context->read_payload == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    for (size_t i = 0u; i < context->manifest.file_count; ++i) {
        const h2_h2loader_host_factory_file_t *file =
            &context->manifest.files[i];
        h2_h2loader_host_sha256_t sha;
        uint8_t digest[32];
        char actual[H2_H2LOADER_HOST_SHA256_HEX_LEN + 1u];
        h2_h2loader_host_sha256_init(&sha);
        for (uint64_t offset = 0u; offset < file->bytes;
             offset += H2_BK7258_SECTOR_SIZE) {
            if (is_cancelled != NULL && is_cancelled(cancel_user)) {
                return H2_PAL_EXIT;
            }
            const uint8_t *data = NULL;
            h2_pal_result_t rc = read_sector(
                context,
                file->flash_offset + (uint32_t)offset,
                &data);
            if (rc != H2_PAL_OK) {
                return rc;
            }
            size_t bytes = file->bytes - offset >
                    H2_BK7258_SECTOR_SIZE
                ? H2_BK7258_SECTOR_SIZE
                : (size_t)(file->bytes - offset);
            h2_h2loader_host_sha256_update(&sha, data, bytes);
        }
        h2_h2loader_host_sha256_finish(&sha, digest);
        h2_h2loader_host_sha256_hex(digest, actual);
        if (strcmp(actual, file->sha256) != 0) {
            return H2_PAL_ERR_FORMAT;
        }
    }
    return H2_PAL_OK;
}

static h2_pal_result_t bk7258_reset(void *user) {
    h2_bk7258_flash_context_t *context = user;
    if (context == NULL || context->read_payload == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    const uint8_t command[] = {
        0x01u,
        0xe0u,
        0xfcu,
        0x02u,
        H2_BK7258_COMMAND_REBOOT,
        0xa5u,
    };
    return write_all(
        context, command, sizeof(command), H2_BK7258_IO_TIMEOUT_MS);
}

static h2_pal_result_t bk7258_close(void *user) {
    h2_bk7258_flash_context_t *context = user;
    if (context == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t rc = h2_pal_serial_host_close(
        context->serial, &context->session);
    h2_pal_mem_free(context->allocator, context);
    return rc;
}

h2_pal_result_t h2_h2loader_host_bk7258_flash_open(
    const h2_h2loader_host_bk7258_flash_config_t *config,
    h2_h2loader_host_flash_driver_t *out_driver) {
    static const h2_h2loader_host_flash_driver_vtable_t vtable = {
        .prepare = bk7258_prepare,
        .erase = bk7258_erase,
        .write = bk7258_write,
        .verify = bk7258_verify,
        .reset_to_loader = bk7258_reset,
        .close = bk7258_close,
    };
    if (out_driver != NULL) {
        memset(out_driver, 0, sizeof(*out_driver));
    }
    if (config == NULL || out_driver == NULL ||
        config->serial == NULL || config->time == NULL ||
        config->allocator == NULL || config->port_id == NULL ||
        config->port_id[0] == '\0' ||
        config->expected_target == NULL ||
        strcmp(config->expected_target, "bk7258") != 0) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_bk7258_flash_context_t *context = h2_pal_mem_alloc(
        config->allocator, sizeof(*context));
    if (context == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(context, 0, sizeof(*context));
    context->serial = config->serial;
    context->time = config->time;
    context->allocator = config->allocator;
    context->connect_attempts = config->connect_attempts == 0u
        ? H2_BK7258_CONNECT_ATTEMPTS
        : config->connect_attempts;
    context->active_baud = H2_BK7258_INITIAL_BAUD;
    const h2_pal_uart_io_stream_config_t uart_config = {
        .baud_rate = H2_BK7258_INITIAL_BAUD,
        .data_bits = 8u,
        .stop_bits = 1u,
        .parity = H2_PAL_UART_PARITY_NONE,
        .flow_control = H2_PAL_UART_FLOW_CONTROL_NONE,
        .rx_buffer_size = 64u * 1024u,
        .tx_buffer_size = 8192u,
    };
    h2_pal_result_t rc = h2_pal_serial_host_open(
        config->serial,
        config->port_id,
        &uart_config,
        &context->session);
    if (rc == H2_PAL_OK) {
        rc = h2_pal_serial_host_session_stream(
            config->serial, context->session, &context->uart);
    }
    if (rc != H2_PAL_OK) {
        (void)h2_pal_serial_host_close(
            config->serial, &context->session);
        h2_pal_mem_free(config->allocator, context);
        return rc;
    }
    out_driver->user = context;
    out_driver->vtable = &vtable;
    return H2_PAL_OK;
}
