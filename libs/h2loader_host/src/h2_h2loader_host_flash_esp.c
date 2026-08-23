#include "h2_h2loader_host_flash.h"

#include "esp_loader.h"
#include "esp_loader_io.h"
#include "h2_h2loader_host_factory.h"
#include "h2_h2loader_host_internal.h"

#include <stdatomic.h>
#include <string.h>

#define H2_ESP_FLASH_BAUD 115200u
#define H2_ESP_FLASH_BLOCK_SIZE 1024u

typedef struct h2_esp_flash_context {
    const h2_pal_serial_host_api_t *serial;
    const h2_pal_time_api_t *time;
    const h2_pal_mem_api_t *allocator;
    h2_pal_serial_host_session_t *session;
    const h2_pal_uart_io_stream_api_t *uart;
    h2_h2loader_host_esp_boot_policy_t boot_policy;
    target_chip_t expected_chip;
    uint64_t timer_deadline_ms;
    h2_h2loader_host_factory_manifest_t manifest;
    h2_h2loader_host_payload_read_fn read_payload;
    void *payload_user;
} h2_esp_flash_context_t;

static h2_esp_flash_context_t *active_context;
static atomic_flag active_context_claim = ATOMIC_FLAG_INIT;

static esp_loader_error_t esp_error(h2_pal_result_t rc) {
    if (rc == H2_PAL_OK) {
        return ESP_LOADER_SUCCESS;
    }
    if (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK) {
        return ESP_LOADER_ERROR_TIMEOUT;
    }
    if (rc == H2_PAL_ERR_INVALID_ARG) {
        return ESP_LOADER_ERROR_INVALID_PARAM;
    }
    return ESP_LOADER_ERROR_FAIL;
}

static target_chip_t expected_chip(const char *target) {
    if (strcmp(target, "esp32s3") == 0) {
        return ESP32S3_CHIP;
    }
    if (strcmp(target, "esp32p4") == 0) {
        return ESP32P4_CHIP;
    }
    if (strcmp(target, "esp32c5") == 0) {
        return ESP32C5_CHIP;
    }
    return ESP_UNKNOWN_CHIP;
}

esp_loader_error_t loader_port_write(
    const uint8_t *data,
    uint16_t size,
    uint32_t timeout) {
    if (active_context == NULL || active_context->uart == NULL) {
        return ESP_LOADER_ERROR_FAIL;
    }
    size_t offset = 0u;
    while (offset < size) {
        size_t written = 0u;
        h2_pal_result_t rc = h2_pal_uart_io_stream_write(
            active_context->uart,
            &data[offset],
            size - offset,
            &written,
            timeout);
        if (rc != H2_PAL_OK) {
            return esp_error(rc);
        }
        if (written == 0u || written > size - offset) {
            return ESP_LOADER_ERROR_FAIL;
        }
        offset += written;
    }
    return ESP_LOADER_SUCCESS;
}

esp_loader_error_t loader_port_read(
    uint8_t *data,
    uint16_t size,
    uint32_t timeout) {
    if (active_context == NULL || active_context->uart == NULL) {
        return ESP_LOADER_ERROR_FAIL;
    }
    size_t offset = 0u;
    while (offset < size) {
        size_t read = 0u;
        h2_pal_result_t rc = h2_pal_uart_io_stream_read(
            active_context->uart,
            &data[offset],
            size - offset,
            &read,
            timeout);
        if (rc != H2_PAL_OK) {
            return esp_error(rc);
        }
        if (read == 0u || read > size - offset) {
            return ESP_LOADER_ERROR_TIMEOUT;
        }
        offset += read;
    }
    return ESP_LOADER_SUCCESS;
}

void loader_port_delay_ms(uint32_t ms) {
    if (active_context != NULL) {
        (void)h2_pal_time_sleep_ms(active_context->time, ms);
    }
}

void loader_port_start_timer(uint32_t ms) {
    uint64_t now = 0u;
    if (active_context != NULL &&
        h2_pal_time_get_monotonic_ms(
            active_context->time, &now) == H2_PAL_OK) {
        active_context->timer_deadline_ms = now + ms;
    }
}

uint32_t loader_port_remaining_time(void) {
    uint64_t now = 0u;
    if (active_context == NULL ||
        h2_pal_time_get_monotonic_ms(
            active_context->time, &now) != H2_PAL_OK ||
        now >= active_context->timer_deadline_ms) {
        return 0u;
    }
    uint64_t remaining = active_context->timer_deadline_ms - now;
    return remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
}

static void set_lines(uint32_t asserted) {
    if (active_context == NULL ||
        active_context->boot_policy !=
            H2_H2LOADER_HOST_ESP_BOOT_DTR_RTS) {
        return;
    }
    (void)h2_pal_serial_host_set_control_lines(
        active_context->serial,
        active_context->session,
        H2_PAL_SERIAL_HOST_CONTROL_DTR |
            H2_PAL_SERIAL_HOST_CONTROL_RTS,
        asserted);
}

void loader_port_enter_bootloader(void) {
    if (active_context == NULL ||
        active_context->boot_policy ==
            H2_H2LOADER_HOST_ESP_BOOT_MANUAL) {
        return;
    }
    set_lines(H2_PAL_SERIAL_HOST_CONTROL_RTS);
    loader_port_delay_ms(50u);
    set_lines(H2_PAL_SERIAL_HOST_CONTROL_DTR);
    loader_port_delay_ms(100u);
    set_lines(0u);
    loader_port_delay_ms(50u);
}

void loader_port_reset_target(void) {
    if (active_context == NULL ||
        active_context->boot_policy ==
            H2_H2LOADER_HOST_ESP_BOOT_MANUAL) {
        return;
    }
    set_lines(H2_PAL_SERIAL_HOST_CONTROL_RTS);
    loader_port_delay_ms(100u);
    set_lines(0u);
    loader_port_delay_ms(50u);
}

void loader_port_debug_print(const char *str) {
    (void)str;
}

esp_loader_error_t loader_port_change_transmission_rate(
    uint32_t transmission_rate) {
    (void)transmission_rate;
    return ESP_LOADER_ERROR_UNSUPPORTED_FUNC;
}

static h2_pal_result_t from_esp(esp_loader_error_t rc) {
    if (rc == ESP_LOADER_SUCCESS) {
        return H2_PAL_OK;
    }
    if (rc == ESP_LOADER_ERROR_TIMEOUT) {
        return H2_PAL_ERR_TIMEOUT;
    }
    if (rc == ESP_LOADER_ERROR_INVALID_PARAM) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (rc == ESP_LOADER_ERROR_UNSUPPORTED_CHIP ||
        rc == ESP_LOADER_ERROR_UNSUPPORTED_FUNC) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (rc == ESP_LOADER_ERROR_INVALID_MD5 ||
        rc == ESP_LOADER_ERROR_INVALID_RESPONSE ||
        rc == ESP_LOADER_ERROR_INVALID_TARGET) {
        return H2_PAL_ERR_FORMAT;
    }
    return H2_PAL_ERR_IO;
}

static h2_pal_result_t esp_flash_prepare(
    void *user,
    const h2_h2loader_host_catalog_entry_t *asset,
    h2_h2loader_host_payload_read_fn read_payload,
    void *payload_user) {
    h2_esp_flash_context_t *context = user;
    if (context == NULL || active_context != context ||
        asset == NULL || read_payload == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t rc = h2_h2loader_host_factory_open(
        asset, read_payload, payload_user, &context->manifest);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (context->manifest.driver !=
        H2_H2LOADER_HOST_FACTORY_DRIVER_ESP_ROM) {
        memset(&context->manifest, 0, sizeof(context->manifest));
        return H2_PAL_ERR_INVALID_STATE;
    }
    context->read_payload = read_payload;
    context->payload_user = payload_user;
    return H2_PAL_OK;
}

static h2_pal_result_t esp_flash_erase(
    void *user,
    h2_h2loader_host_cancelled_fn is_cancelled,
    void *cancel_user) {
    h2_esp_flash_context_t *context = user;
    if (context == NULL || active_context != context) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (is_cancelled != NULL && is_cancelled(cancel_user)) {
        return H2_PAL_EXIT;
    }
    esp_loader_connect_args_t connect_args =
        ESP_LOADER_CONNECT_DEFAULT();
    h2_pal_result_t rc =
        from_esp(esp_loader_connect_with_stub(&connect_args));
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (esp_loader_get_target() != context->expected_chip) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    return from_esp(esp_loader_flash_erase());
}

static h2_pal_result_t esp_flash_write(
    void *user,
    const h2_h2loader_host_catalog_entry_t *asset,
    h2_h2loader_host_payload_read_fn read_payload,
    void *payload_user,
    h2_h2loader_host_cancelled_fn is_cancelled,
    void *cancel_user,
    h2_h2loader_host_progress_fn on_progress,
    void *progress_user) {
    h2_esp_flash_context_t *context = user;
    uint64_t total = 0u;
    uint64_t written_total = 0u;
    uint8_t block[H2_ESP_FLASH_BLOCK_SIZE];
    if (context == NULL || active_context != context) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (context->read_payload != read_payload ||
        context->payload_user != payload_user ||
        context->manifest.driver !=
            H2_H2LOADER_HOST_FACTORY_DRIVER_ESP_ROM ||
        strcmp(context->manifest.board, asset->board) != 0 ||
        strcmp(context->manifest.target, asset->target) != 0) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    h2_pal_result_t rc = H2_PAL_OK;
    for (size_t i = 0u; i < context->manifest.file_count; ++i) {
        total += context->manifest.files[i].bytes;
    }
    for (size_t i = 0u; i < context->manifest.file_count; ++i) {
        const h2_h2loader_host_factory_file_t *file =
            &context->manifest.files[i];
        if (file->bytes > UINT32_MAX) {
            return H2_PAL_ERR_NO_SPACE;
        }
        rc = from_esp(esp_loader_flash_start(
            file->flash_offset,
            (uint32_t)file->bytes,
            sizeof(block)));
        for (uint64_t offset = 0u;
             rc == H2_PAL_OK && offset < file->bytes;) {
            if (is_cancelled != NULL && is_cancelled(cancel_user)) {
                return H2_PAL_EXIT;
            }
            size_t read = 0u;
            size_t request = file->bytes - offset > sizeof(block)
                ? sizeof(block)
                : (size_t)(file->bytes - offset);
            rc = h2_h2loader_host_factory_read_member(
                &context->manifest,
                i,
                read_payload,
                payload_user,
                offset,
                block,
                request,
                &read);
            if (rc != H2_PAL_OK || read != request) {
                return rc == H2_PAL_OK ? H2_PAL_ERR_TRUNCATED : rc;
            }
            rc = from_esp(esp_loader_flash_write(block, (uint32_t)read));
            offset += read;
            written_total += read;
            if (rc == H2_PAL_OK && on_progress != NULL) {
                on_progress(progress_user, written_total, total);
            }
        }
        if (rc != H2_PAL_OK) {
            return rc;
        }
        rc = from_esp(esp_loader_flash_finish(false));
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    return H2_PAL_OK;
}

static h2_pal_result_t esp_flash_verify(
    void *user,
    h2_h2loader_host_cancelled_fn is_cancelled,
    void *cancel_user) {
    h2_esp_flash_context_t *context = user;
    uint8_t buffer[8192];
    if (context == NULL || active_context != context ||
        context->read_payload == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    for (size_t i = 0u; i < context->manifest.file_count; ++i) {
        const h2_h2loader_host_factory_file_t *file =
            &context->manifest.files[i];
        h2_h2loader_host_sha256_t sha;
        uint8_t digest[32];
        char actual[H2_H2LOADER_HOST_SHA256_HEX_LEN + 1u];
        h2_h2loader_host_sha256_init(&sha);
        for (uint64_t offset = 0u; offset < file->bytes;) {
            if (is_cancelled != NULL && is_cancelled(cancel_user)) {
                return H2_PAL_EXIT;
            }
            size_t request = file->bytes - offset > sizeof(buffer)
                ? sizeof(buffer)
                : (size_t)(file->bytes - offset);
            if (offset > UINT32_MAX ||
                (uint64_t)file->flash_offset + offset > UINT32_MAX ||
                request > UINT32_MAX) {
                return H2_PAL_ERR_NO_SPACE;
            }
            h2_pal_result_t rc = from_esp(esp_loader_flash_read(
                buffer,
                file->flash_offset + (uint32_t)offset,
                (uint32_t)request));
            if (rc != H2_PAL_OK) {
                return rc;
            }
            h2_h2loader_host_sha256_update(&sha, buffer, request);
            offset += request;
        }
        h2_h2loader_host_sha256_finish(&sha, digest);
        h2_h2loader_host_sha256_hex(digest, actual);
        if (strcmp(actual, file->sha256) != 0) {
            return H2_PAL_ERR_FORMAT;
        }
    }
    return H2_PAL_OK;
}

static h2_pal_result_t esp_flash_reset(void *user) {
    if (user == NULL || active_context != user) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    esp_loader_reset_target();
    return H2_PAL_OK;
}

static h2_pal_result_t esp_flash_close(void *user) {
    h2_esp_flash_context_t *context = user;
    if (context == NULL || active_context != context) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    active_context = NULL;
    h2_pal_result_t rc = h2_pal_serial_host_close(
        context->serial, &context->session);
    h2_pal_mem_free(context->allocator, context);
    atomic_flag_clear_explicit(
        &active_context_claim, memory_order_release);
    return rc;
}

h2_pal_result_t h2_h2loader_host_esp_flash_open(
    const h2_h2loader_host_esp_flash_config_t *config,
    h2_h2loader_host_flash_driver_t *out_driver) {
    static const h2_h2loader_host_flash_driver_vtable_t vtable = {
        .prepare = esp_flash_prepare,
        .erase = esp_flash_erase,
        .write = esp_flash_write,
        .verify = esp_flash_verify,
        .reset_to_loader = esp_flash_reset,
        .close = esp_flash_close,
    };
    if (out_driver != NULL) {
        memset(out_driver, 0, sizeof(*out_driver));
    }
    if (config == NULL || out_driver == NULL ||
        config->serial == NULL || config->time == NULL ||
        config->allocator == NULL || config->port_id == NULL ||
        config->expected_target == NULL ||
        (config->boot_policy != H2_H2LOADER_HOST_ESP_BOOT_MANUAL &&
         config->boot_policy != H2_H2LOADER_HOST_ESP_BOOT_DTR_RTS)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    target_chip_t chip = expected_chip(config->expected_target);
    if (chip == ESP_UNKNOWN_CHIP) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (atomic_flag_test_and_set_explicit(
            &active_context_claim, memory_order_acquire)) {
        return H2_PAL_ERR_UNAVAILABLE;
    }
    h2_esp_flash_context_t *context = h2_pal_mem_alloc(
        config->allocator, sizeof(*context));
    if (context == NULL) {
        atomic_flag_clear_explicit(
            &active_context_claim, memory_order_release);
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(context, 0, sizeof(*context));
    context->serial = config->serial;
    context->time = config->time;
    context->allocator = config->allocator;
    context->boot_policy = config->boot_policy;
    context->expected_chip = chip;
    const h2_pal_uart_io_stream_config_t uart_config = {
        .baud_rate = H2_ESP_FLASH_BAUD,
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
        atomic_flag_clear_explicit(
            &active_context_claim, memory_order_release);
        return rc;
    }
    active_context = context;
    out_driver->user = context;
    out_driver->vtable = &vtable;
    return H2_PAL_OK;
}
