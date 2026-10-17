#ifndef H2_ESP_E2E_RESULT_CONSOLE_H
#define H2_ESP_E2E_RESULT_CONSOLE_H

#include "h2_atomic.h"
#include "h2/pal/hal/h2_pal_uart_io_stream.h"
#include "h2/pal/hal/h2_pal_usb_jtag_io_stream.h"
#include "h2/pal/os/h2_pal_log.h"
#include "h2/pal/os/h2_pal_sync.h"
#include "h2/pal/os/h2_pal_time.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* Borrow the console already installed by H2Loader. The caller serializes
 * access; MQTT's single runner and the result router below share this sink. */
typedef struct h2_esp_e2e_console {
    const h2_pal_usb_jtag_io_stream_api_t *usb;
    const h2_pal_uart_io_stream_api_t *uart;
    uint32_t timeout_ms;
    int error;
    const h2_pal_time_api_t *clock;
} h2_esp_e2e_console_t;

static inline int h2_esp_e2e_console_record(h2_esp_e2e_console_t *console,
                                          const char *message) {
    if (console == NULL) return H2_PAL_ERR_INVALID_ARG;
    if (console->error != H2_PAL_OK) return console->error;
    if (message == NULL || console->timeout_ms == 0u)
        return console->error = H2_PAL_ERR_INVALID_ARG;
    char record[1024];
    size_t length = 0u;
    while (length < sizeof(record) && message[length] != '\0') ++length;
    if (length == sizeof(record)) return console->error = H2_PAL_ERR_TRUNCATED;
    /* Existing runners optionally append one line ending. Never accept an
     * interior line break or a second line as another successful record. */
    if (length && message[length - 1u] == '\n') {
        --length;
        if (length && message[length - 1u] == '\r') --length;
    }
    if (length == 0u || length > sizeof(record) - 2u)
        return console->error = H2_PAL_ERR_INVALID_ARG;
    for (size_t i = 0u; i < length; ++i)
        if (message[i] == '\r' || message[i] == '\n')
            return console->error = H2_PAL_ERR_INVALID_ARG;
    size_t prefix = 0u;
    if (console->clock != NULL) {
        uint64_t ms = 0u;
        int rc = h2_pal_time_get_monotonic_ms(console->clock, &ms);
        if (rc != H2_PAL_OK) return console->error = rc;
        int count = snprintf(record, sizeof(record),
            "H2_E2E_RESULT source=pal_monotonic_ms time_ms=%" PRIu64 " ", ms);
        if (count < 0 || (size_t)count >= sizeof(record) - 2u)
            return console->error = H2_PAL_ERR_TRUNCATED;
        prefix = (size_t)count;
        if (length > sizeof(record) - 2u - prefix)
            return console->error = H2_PAL_ERR_TRUNCATED;
    }
    memcpy(record + prefix, message, length);
    length += prefix;
    record[length++] = '\r';
    record[length++] = '\n';
    size_t written = 0u;
    if ((console->usb == NULL) == (console->uart == NULL))
        return console->error = H2_PAL_ERR_INVALID_ARG;
    int rc = console->usb != NULL
        ? h2_pal_usb_jtag_io_stream_write(console->usb, record, length, &written, console->timeout_ms)
        : h2_pal_uart_io_stream_write(console->uart, record, length, &written, console->timeout_ms);
    if (rc == H2_PAL_OK && written != length) rc = H2_PAL_ERR_IO;
    if (rc == H2_PAL_OK) rc = console->usb != NULL
        ? h2_pal_usb_jtag_io_stream_flush(console->usb)
        : h2_pal_uart_io_stream_flush(console->uart);
    if (rc != H2_PAL_OK) console->error = rc;
    return rc;
}

/* Boot-owned router. Result scopes are borrowed stable strings; all other
 * records retain the original Log's filtering and provider behavior. */
typedef struct h2_esp_e2e_result_log {
    h2_esp_e2e_console_t console;
    const h2_pal_log_api_t *delegate;
    const h2_pal_sync_api_t *sync;
    h2_pal_mutex_t *mutex;
    h2_atomic_int_t error;
    const char *scope;
    const char *second_scope;
    size_t records;
    h2_pal_log_api_t log;
} h2_esp_e2e_result_log_t;

static inline int h2_esp_e2e_result_log_remember(h2_esp_e2e_result_log_t *state, int rc) {
    if (rc != H2_PAL_OK) {
        int expected = H2_PAL_OK;
        (void)h2_atomic_int_compare_exchange(&state->error, &expected, rc,
            H2_ATOMIC_SEQ_CST, H2_ATOMIC_SEQ_CST);
    }
    return h2_atomic_int_load(&state->error, H2_ATOMIC_SEQ_CST);
}

static inline int h2_esp_e2e_result_log_error(h2_esp_e2e_result_log_t *state) {
    if (state == NULL || state->error.storage == NULL) return H2_PAL_ERR_INVALID_STATE;
    return h2_esp_e2e_result_log_remember(state, H2_PAL_OK);
}

/* Opt in once, before the first result. The clock is borrowed for the boot;
 * failure is terminal, without a zero/host-time substitute. Other scopes and
 * callers that do not opt in retain their original wire format. */
static inline int h2_esp_e2e_result_log_use_clock(h2_esp_e2e_result_log_t *state,
                                               const h2_pal_time_api_t *clock) {
    int rc = h2_esp_e2e_result_log_error(state);
    if (rc != H2_PAL_OK) return rc;
    if (clock == NULL || clock->vtable == NULL || clock->vtable->get_monotonic_ms == NULL)
        return h2_esp_e2e_result_log_remember(state, H2_PAL_ERR_UNSUPPORTED);
    rc = h2_pal_mutex_lock(state->sync, state->mutex);
    if (rc != H2_PAL_OK) return h2_esp_e2e_result_log_remember(state, rc);
    if (state->records != 0u || state->console.clock != NULL)
        (void)h2_esp_e2e_result_log_remember(state, H2_PAL_ERR_INVALID_STATE);
    else state->console.clock = clock;
    rc = h2_pal_mutex_unlock(state->sync, state->mutex);
    return h2_esp_e2e_result_log_remember(state, rc);
}

static inline int h2_esp_e2e_result_log_write(void *user, h2_pal_log_level_t level,
                                            const char *scope, const char *message) {
    h2_esp_e2e_result_log_t *state = user;
    if (state == NULL || state->error.storage == NULL) return H2_PAL_ERR_INVALID_STATE;
    if (scope == NULL || (strcmp(scope, state->scope) != 0 &&
        (state->second_scope == NULL || strcmp(scope, state->second_scope) != 0)))
        return h2_pal_log_write(state->delegate, level, scope, message);
    if (level != H2_PAL_LOG_DEBUG && level != H2_PAL_LOG_INFO &&
        level != H2_PAL_LOG_WARN && level != H2_PAL_LOG_ERROR)
        return h2_esp_e2e_result_log_remember(state, H2_PAL_ERR_INVALID_ARG);
    int rc = h2_esp_e2e_result_log_error(state);
    if (rc != H2_PAL_OK) return rc;
    rc = h2_pal_mutex_lock(state->sync, state->mutex);
    if (rc != H2_PAL_OK) return h2_esp_e2e_result_log_remember(state, rc);
    if (h2_esp_e2e_result_log_error(state) == H2_PAL_OK) {
        rc = h2_esp_e2e_console_record(&state->console, message);
        if (rc == H2_PAL_OK) {
            if (state->records == SIZE_MAX) rc = H2_PAL_ERR_INVALID_STATE;
            else ++state->records;
        }
        (void)h2_esp_e2e_result_log_remember(state, rc);
    }
    rc = h2_pal_mutex_unlock(state->sync, state->mutex);
    return h2_esp_e2e_result_log_remember(state, rc);
}

/* Initialize zeroed caller storage before publishing log. The supplied PALs
 * are borrowed for the boot; init failure releases its own partial state. */
static inline int h2_esp_e2e_result_log_init(h2_esp_e2e_result_log_t *state,
    const h2_pal_log_api_t *delegate, const h2_pal_sync_api_t *sync,
    const h2_pal_uart_io_stream_api_t *uart, const h2_pal_usb_jtag_io_stream_api_t *usb,
    const char *scope, const char *second_scope) {
    if (state == NULL || delegate == NULL || delegate == &state->log || sync == NULL ||
        scope == NULL || scope[0] == '\0' || (uart == NULL) == (usb == NULL) ||
        (second_scope != NULL && (second_scope[0] == '\0' || strcmp(scope, second_scope) == 0)))
        return H2_PAL_ERR_INVALID_ARG;
    if (state->error.storage != NULL || state->mutex != NULL) return H2_PAL_ERR_INVALID_STATE;
    if (delegate->vtable == NULL || delegate->vtable->write == NULL ||
        sync->vtable == NULL || sync->vtable->create_mutex == NULL ||
        sync->vtable->destroy_mutex == NULL || sync->vtable->lock_mutex == NULL || sync->vtable->unlock_mutex == NULL ||
        (uart != NULL && (uart->vtable == NULL || uart->vtable->write == NULL || uart->vtable->flush == NULL)) ||
        (usb != NULL && (usb->vtable == NULL || usb->vtable->write == NULL || usb->vtable->flush == NULL)))
        return H2_PAL_ERR_UNSUPPORTED;
    int atomic = h2_atomic_int_init(&state->error, H2_PAL_OK);
    if (atomic != H2_ATOMIC_OK)
        return atomic == H2_ATOMIC_NO_MEMORY ? H2_PAL_ERR_NO_MEMORY : H2_PAL_ERR_UNSUPPORTED;
    const h2_pal_mutex_config_t config = {.name = "e2e/result"};
    int rc = h2_pal_mutex_create(sync, &config, &state->mutex);
    if (rc != H2_PAL_OK || state->mutex == NULL) {
        h2_atomic_int_destroy(&state->error);
        state->mutex = NULL;
        return rc != H2_PAL_OK ? rc : H2_PAL_ERR_INVALID_STATE;
    }
    state->console = (h2_esp_e2e_console_t){.usb = usb, .uart = uart, .timeout_ms = 5000u};
    state->delegate = delegate;
    state->sync = sync;
    state->scope = scope;
    state->second_scope = second_scope;
    static const h2_pal_log_vtable_t vtable = {.write = h2_esp_e2e_result_log_write};
    state->log = (h2_pal_log_api_t){.user = state, .vtable = &vtable};
    return H2_PAL_OK;
}

/* All suite callbacks and first-report writes must have returned before this
 * gate. A later replay never clears the first failed write or drain. */
static inline size_t h2_esp_e2e_result_log_record_count(h2_esp_e2e_result_log_t *state) {
    if (h2_esp_e2e_result_log_error(state) != H2_PAL_OK) return 0u;
    int rc = h2_pal_mutex_lock(state->sync, state->mutex);
    if (rc != H2_PAL_OK) {
        (void)h2_esp_e2e_result_log_remember(state, rc);
        return 0u;
    }
    size_t count = state->records;
    rc = h2_pal_mutex_unlock(state->sync, state->mutex);
    (void)h2_esp_e2e_result_log_remember(state, rc);
    return count;
}

static inline int h2_esp_e2e_result_log_confirm(h2_esp_e2e_result_log_t *state,
    int suite_rc, size_t expected_records, int (*confirm)(void *), void *user) {
    if (suite_rc != H2_PAL_OK) return suite_rc;
    int rc = h2_esp_e2e_result_log_error(state);
    if (rc != H2_PAL_OK) return rc;
    if (expected_records && h2_esp_e2e_result_log_record_count(state) != expected_records)
        return h2_esp_e2e_result_log_remember(state, H2_PAL_ERR_INVALID_STATE);
    rc = h2_esp_e2e_result_log_error(state);
    if (rc != H2_PAL_OK) return rc;
    return confirm != NULL ? confirm(user) : H2_PAL_ERR_INVALID_ARG;
}

/* Only after every writer has stopped. Infinite-hold firmware entries retain
 * the router until reboot; tests and finite owners release it explicitly. */
static inline int h2_esp_e2e_result_log_deinit(h2_esp_e2e_result_log_t *state) {
    if (state == NULL) return H2_PAL_ERR_INVALID_ARG;
    if (state->mutex != NULL) {
        int rc = h2_pal_mutex_destroy(state->sync, state->mutex);
        if (rc != H2_PAL_OK) return rc;
    }
    if (state->error.storage != NULL) h2_atomic_int_destroy(&state->error);
    memset(state, 0, sizeof(*state));
    return H2_PAL_OK;
}

#if defined(ESP_PLATFORM)
#include "h2_esp_platform_core.h"
#include "h2_runtime.h"
#include "sdkconfig.h"

static inline int h2_esp_e2e_result_log_prepare(h2_esp_e2e_result_log_t *state,
    h2_runtime_t *runtime, const char *scope, const char *second_scope) {
    if (runtime == NULL) return H2_PAL_ERR_INVALID_ARG;
    const h2_pal_uart_io_stream_api_t *uart = NULL;
    const h2_pal_usb_jtag_io_stream_api_t *usb = NULL;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb = h2_esp_platform_usb_jtag_io_stream_api();
#else
    uart = h2_esp_platform_uart_io_stream_api();
#endif
    int rc = h2_esp_e2e_result_log_init(state, runtime->log, runtime->sync,
        uart, usb, scope, second_scope);
    if (rc == H2_PAL_OK) runtime->log = &state->log;
    return rc;
}
#endif

#endif
