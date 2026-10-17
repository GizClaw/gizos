#include "result_console.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#if defined(_WIN32)
#include "h2_windows_platform.h"
#else
#include "h2_desktop_platform.h"
#endif
#include <stdio.h>

static const h2_pal_sync_api_t *native_sync;
static const h2_pal_task_api_t *native_task;
static const h2_pal_time_api_t *native_time;
typedef struct observed {
    h2_atomic_uint_t locks, unlocks;
    h2_atomic_int_t writing;
    unsigned writes, drains, uart_writes, usb_writes, confirms, delegated, visible;
    unsigned fail_write, short_write, fail_drain, fail_lock, fail_unlock;
    int create_error, null_mutex, delegate_error, confirm_error;
    unsigned clock_reads;
    int clock_error;
    size_t length, drained;
    char bytes[16384];
} observed_t;

static int create_mutex(void *user, const h2_pal_mutex_config_t *config, h2_pal_mutex_t **out) {
    observed_t *s = user;
    assert(config != NULL && strcmp(config->name, "e2e/result") == 0);
    *out = NULL;
    if (s->create_error) return s->create_error;
    if (s->null_mutex) return H2_PAL_OK;
    return h2_pal_mutex_create(native_sync, config, out);
}
static int destroy_mutex(void *user, h2_pal_mutex_t *mutex) {
    observed_t *s = user;
    assert(h2_atomic_int_load(&s->writing, H2_ATOMIC_SEQ_CST) == 0);
    return h2_pal_mutex_destroy(native_sync, mutex);
}
static int lock_mutex(void *user, h2_pal_mutex_t *mutex) {
    observed_t *s = user;
    unsigned call = h2_atomic_uint_fetch_add(&s->locks, 1u, H2_ATOMIC_SEQ_CST) + 1u;
    if (call == s->fail_lock) return H2_PAL_ERR_TIMEOUT;
    return h2_pal_mutex_lock(native_sync, mutex);
}
static int unlock_mutex(void *user, h2_pal_mutex_t *mutex) {
    observed_t *s = user;
    unsigned call = h2_atomic_uint_fetch_add(&s->unlocks, 1u, H2_ATOMIC_SEQ_CST) + 1u;
    assert(h2_pal_mutex_unlock(native_sync, mutex) == H2_PAL_OK);
    return call == s->fail_unlock ? H2_PAL_ERR_IO : H2_PAL_OK;
}
static int write_record(void *user, const void *data, size_t length, size_t *written, uint32_t timeout) {
    observed_t *s = user;
    assert(timeout == 5000u && h2_atomic_int_fetch_add(&s->writing, 1, H2_ATOMIC_SEQ_CST) == 0);
    ++s->writes;
    *written = 0u;
    if (s->writes == s->fail_write) {
        assert(h2_atomic_int_fetch_sub(&s->writing, 1, H2_ATOMIC_SEQ_CST) == 1);
        return H2_PAL_ERR_TIMEOUT;
    }
    if (s->writes == s->short_write) length = 3u;
    assert(s->length + length < sizeof(s->bytes));
    for (size_t i = 0u; i < length; ++i) {
        s->bytes[s->length++] = ((const char *)data)[i];
        assert(h2_pal_time_sleep_ms(native_time, 0u) == H2_PAL_OK);
    }
    s->bytes[s->length] = '\0';
    *written = length;
    assert(h2_atomic_int_fetch_sub(&s->writing, 1, H2_ATOMIC_SEQ_CST) == 1);
    return H2_PAL_OK;
}
static int write_uart(void *user, const void *data, size_t length, size_t *written, uint32_t timeout) {
    observed_t *s = user;
    ++s->uart_writes;
    return write_record(user, data, length, written, timeout);
}
static int write_usb(void *user, const void *data, size_t length, size_t *written, uint32_t timeout) {
    observed_t *s = user;
    ++s->usb_writes;
    return write_record(user, data, length, written, timeout);
}
static int drain(void *user) {
    observed_t *s = user;
    ++s->drains;
    if (s->drains == s->fail_drain) return H2_PAL_ERR_TIMEOUT;
    s->drained = s->length;
    return H2_PAL_OK;
}
static int delegate(void *user, h2_pal_log_level_t level, const char *scope, const char *message) {
    observed_t *s = user;
    (void)scope;
    assert(message != NULL);
    ++s->delegated;
    if (level != H2_PAL_LOG_DEBUG && level != H2_PAL_LOG_INFO &&
        level != H2_PAL_LOG_WARN && level != H2_PAL_LOG_ERROR)
        return H2_PAL_ERR_INVALID_ARG;
    if (level >= H2_PAL_LOG_WARN) ++s->visible;
    return s->delegate_error;
}
static int confirm(void *user) {
    observed_t *s = user;
    assert(s->length == s->drained && s->writes == s->drains);
    ++s->confirms;
    return s->confirm_error;
}
static int monotonic_ms(void *user, uint64_t *out) {
    observed_t *s = user;
    ++s->clock_reads;
    if (s->clock_error) return s->clock_error;
    return h2_pal_time_get_monotonic_ms(native_time, out);
}
static const h2_pal_time_vtable_t clock_vtable = {.get_monotonic_ms = monotonic_ms};
static const h2_pal_sync_vtable_t sync_vtable = {
    .create_mutex = create_mutex, .destroy_mutex = destroy_mutex,
    .lock_mutex = lock_mutex, .unlock_mutex = unlock_mutex,
};
static const h2_pal_uart_io_stream_vtable_t uart_vtable = {.write = write_uart, .flush = drain};
static const h2_pal_usb_jtag_io_stream_vtable_t usb_vtable = {.write = write_usb, .flush = drain};
static const h2_pal_log_vtable_t log_vtable = {.write = delegate};
typedef struct fixture {
    observed_t s;
    h2_pal_sync_api_t sync;
    h2_pal_uart_io_stream_api_t uart;
    h2_pal_usb_jtag_io_stream_api_t usb;
    h2_pal_log_api_t original;
    h2_esp_e2e_result_log_t router;
} fixture_t;
static int prepare(fixture_t *f, int usb) {
    assert(h2_atomic_uint_init(&f->s.locks, 0u) == H2_ATOMIC_OK);
    assert(h2_atomic_uint_init(&f->s.unlocks, 0u) == H2_ATOMIC_OK);
    assert(h2_atomic_int_init(&f->s.writing, 0) == H2_ATOMIC_OK);
    f->sync = (h2_pal_sync_api_t){.user = &f->s, .vtable = &sync_vtable};
    f->uart = (h2_pal_uart_io_stream_api_t){.user = &f->s, .vtable = &uart_vtable};
    f->usb = (h2_pal_usb_jtag_io_stream_api_t){.user = &f->s, .vtable = &usb_vtable};
    f->original = (h2_pal_log_api_t){.user = &f->s, .vtable = &log_vtable};
    return h2_esp_e2e_result_log_init(&f->router, &f->original, &f->sync,
        usb ? NULL : &f->uart, usb ? &f->usb : NULL, "pal-http", "iperf_e2e");
}
static int emit(fixture_t *f, const char *message) {
    return h2_pal_log_write(&f->router.log, H2_PAL_LOG_INFO, "pal-http", message);
}
static int finish(fixture_t *f, int suite, size_t expected) {
    return h2_esp_e2e_result_log_confirm(&f->router, suite, expected, confirm, &f->s);
}
static void close_fixture(fixture_t *f) {
    assert(h2_esp_e2e_result_log_deinit(&f->router) == H2_PAL_OK);
    assert(h2_esp_e2e_result_log_deinit(&f->router) == H2_PAL_OK);
    h2_atomic_uint_destroy(&f->s.locks);
    h2_atomic_uint_destroy(&f->s.unlocks);
    h2_atomic_int_destroy(&f->s.writing);
}
static void framing_and_delegate(int usb) {
    fixture_t f = {0};
    assert(prepare(&f, usb) == H2_PAL_OK);
    assert(emit(&f, "CASE {}\n") == H2_PAL_OK);
    assert(emit(&f, "SUMMARY {}\r\n") == H2_PAL_OK);
    assert(h2_pal_log_write(&f.router.log, H2_PAL_LOG_INFO, "iperf_e2e", "COMPLETE {}") == 0);
    assert(strcmp(f.s.bytes, "CASE {}\r\nSUMMARY {}\r\nCOMPLETE {}\r\n") == 0);
    assert(f.s.writes == 3u && f.s.drains == 3u &&
        (usb ? f.s.usb_writes == 3u && f.s.uart_writes == 0u : f.s.uart_writes == 3u && f.s.usb_writes == 0u));
    assert(h2_pal_log_write(&f.router.log, H2_PAL_LOG_INFO, "pal-http-extra", "hidden") == 0);
    assert(h2_pal_log_write(&f.router.log, H2_PAL_LOG_WARN, "provider", "visible") == 0);
    assert(h2_pal_log_write(&f.router.log, H2_PAL_LOG_INFO, NULL, "hidden") == 0);
    assert(f.s.delegated == 3u && f.s.visible == 1u && f.s.writes == 3u);
    assert(f.original.vtable == &log_vtable && f.router.delegate == &f.original);
    assert(h2_pal_log_write(&f.router.log, (h2_pal_log_level_t)-1, "provider", "invalid level") == H2_PAL_ERR_INVALID_ARG);
    assert(h2_esp_e2e_result_log_error(&f.router) == 0 && f.s.writes == 3u);
    f.s.delegate_error = H2_PAL_ERR_IO;
    assert(h2_pal_log_write(&f.router.log, H2_PAL_LOG_ERROR, "provider", "actual error") == H2_PAL_ERR_IO);
    assert(h2_esp_e2e_result_log_error(&f.router) == 0);
    assert(finish(&f, 0, 3u) == 0 && f.s.confirms == 1u);
    close_fixture(&f);
}
static void failures(int usb) {
    for (unsigned boundary = 1u; boundary <= 3u; ++boundary) {
        for (unsigned kind = 0u; kind < 3u; ++kind) {
            fixture_t f = {0};
            if (kind == 0u) f.s.fail_write = boundary;
            if (kind == 1u) f.s.short_write = boundary;
            if (kind == 2u) f.s.fail_drain = boundary;
            assert(prepare(&f, usb) == 0);
            for (unsigned row = 0u; row < 3u; ++row) (void)emit(&f, "record");
            int expected = kind == 1u ? H2_PAL_ERR_IO : H2_PAL_ERR_TIMEOUT;
            assert(finish(&f, 0, 3u) == expected && f.s.confirms == 0u);
            unsigned writes = f.s.writes;
            assert(emit(&f, "later replay") == expected && f.s.writes == writes);
            assert(h2_pal_log_write(&f.router.log, H2_PAL_LOG_WARN, "provider", "still delegated") == 0);
            close_fixture(&f);
        }
    }
    const char *invalid[] = {"", "\n", "\r\n", "one\r", "one\n\n", "one\r\ntwo", NULL};
    for (size_t i = 0u; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        fixture_t f = {0}; assert(prepare(&f, usb) == 0);
        assert(f.router.log.vtable->write(f.router.log.user, H2_PAL_LOG_INFO, "pal-http", invalid[i]) == H2_PAL_ERR_INVALID_ARG);
        assert(finish(&f, 0, 0u) == H2_PAL_ERR_INVALID_ARG && f.s.writes == 0u && f.s.confirms == 0u);
        close_fixture(&f);
    }
    const int invalid_levels[] = {-1, H2_PAL_LOG_ERROR + 1};
    for (size_t i = 0u; i < sizeof(invalid_levels) / sizeof(invalid_levels[0]); ++i) {
        fixture_t f = {0}; assert(prepare(&f, usb) == 0);
        assert(h2_pal_log_write(&f.router.log, (h2_pal_log_level_t)invalid_levels[i],
            "pal-http", "invalid level") == H2_PAL_ERR_INVALID_ARG);
        assert(finish(&f, 0, 0u) == H2_PAL_ERR_INVALID_ARG && f.s.writes == 0u && f.s.confirms == 0u);
        assert(emit(&f, "later replay") == H2_PAL_ERR_INVALID_ARG && f.s.writes == 0u);
        close_fixture(&f);
    }
    fixture_t f = {0}; assert(prepare(&f, usb) == 0);
    assert(emit(&f, "incomplete first report") == 0);
    assert(finish(&f, 0, 2u) == H2_PAL_ERR_INVALID_STATE && f.s.confirms == 0u);
    assert(emit(&f, "missing row cannot be repaired by replay") == H2_PAL_ERR_INVALID_STATE);
    close_fixture(&f);
    fixture_t bad_suite = {0}; assert(prepare(&bad_suite, usb) == 0);
    assert(emit(&bad_suite, "actual failed suite") == 0);
    assert(finish(&bad_suite, H2_PAL_ERR_IO, 1u) == H2_PAL_ERR_IO && bad_suite.s.confirms == 0u);
    close_fixture(&bad_suite);
    fixture_t bad_confirm = {0}; assert(prepare(&bad_confirm, usb) == 0);
    bad_confirm.s.confirm_error = H2_PAL_ERR_IO;
    assert(finish(&bad_confirm, 0, 0u) == H2_PAL_ERR_IO && bad_confirm.s.confirms == 1u);
    close_fixture(&bad_confirm);
}
typedef struct writer { fixture_t *f; unsigned id; int allow_failure; } writer_t;
static void write_concurrently(void *user) {
    writer_t *writer = user;
    for (unsigned i = 0u; i < 50u; ++i) {
        char record[64];
        (void)snprintf(record, sizeof(record), "writer=%u row=%u", writer->id, i);
        int rc = emit(writer->f, record);
        assert(rc == 0 || (writer->allow_failure && rc == H2_PAL_ERR_TIMEOUT));
    }
}
static void concurrent_records(void) {
    fixture_t f = {0}; assert(prepare(&f, 0) == 0);
    writer_t writers[2] = {{&f, 0u, 0}, {&f, 1u, 0}};
    h2_pal_task_t *threads[2] = {NULL, NULL};
    const h2_pal_task_options_t options = {.name = "e2e/result-test", .min_stack_size = 65536u};
    for (unsigned i = 0u; i < 2u; ++i)
        assert(h2_pal_task_start(native_task, &options, write_concurrently, &writers[i], &threads[i]) == 0);
    for (unsigned i = 0u; i < 2u; ++i) assert(h2_pal_task_join(native_task, threads[i]) == 0);
    unsigned next[2] = {0u, 0u};
    const char *line = f.s.bytes;
    for (unsigned count = 0u; count < 100u; ++count) {
        unsigned id, row; int end = 0;
        assert(sscanf(line, "writer=%u row=%u%n", &id, &row, &end) == 2);
        assert(id < 2u && row == next[id]++ && line[end] == '\r' && line[end + 1] == '\n');
        line += (size_t)end + 2u;
    }
    assert(*line == '\0' && next[0] == 50u && next[1] == 50u);
    assert(finish(&f, 0, 100u) == 0 && f.s.confirms == 1u);
    close_fixture(&f);
}
static void bounds_and_capabilities(void) {
    char message[1025]; memset(message, 'a', sizeof(message));
    message[1022] = '\n'; message[1023] = '\0';
    fixture_t f = {0}; assert(prepare(&f, 0) == 0);
    assert(emit(&f, message) == 0 && f.s.length == 1024u && f.s.drains == 1u);
    message[1022] = message[1023] = 'a'; message[1024] = '\0';
    assert(emit(&f, message) == H2_PAL_ERR_TRUNCATED);
    assert(finish(&f, 0, 0u) == H2_PAL_ERR_TRUNCATED && f.s.confirms == 0u);
    close_fixture(&f);
    fixture_t missing = {0}; assert(prepare(&missing, 0) == 0);
    h2_esp_e2e_result_log_t unused = {0};
    const h2_pal_uart_io_stream_api_t no_operations = {0};
    assert(h2_esp_e2e_result_log_init(&unused, &missing.original, &missing.sync,
        &no_operations, NULL, "pal-http", NULL) == H2_PAL_ERR_UNSUPPORTED);
    assert(h2_esp_e2e_result_log_init(&unused, &missing.original, &missing.sync,
        &missing.uart, &missing.usb, "pal-http", NULL) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_esp_e2e_result_log_init(&unused, &missing.original, &missing.sync,
        NULL, NULL, "pal-http", NULL) == H2_PAL_ERR_INVALID_ARG);
    assert(unused.error.storage == NULL && unused.mutex == NULL);
    close_fixture(&missing);
}
static void concurrent_failure(void) {
    fixture_t f = {0}; assert(prepare(&f, 0) == 0);
    f.s.fail_write = 7u;
    writer_t writers[2] = {{&f, 0u, 1}, {&f, 1u, 1}};
    h2_pal_task_t *threads[2] = {NULL, NULL};
    const h2_pal_task_options_t options = {.name = "e2e/result-test", .min_stack_size = 65536u};
    for (unsigned i = 0u; i < 2u; ++i)
        assert(h2_pal_task_start(native_task, &options, write_concurrently, &writers[i], &threads[i]) == 0);
    for (unsigned i = 0u; i < 2u; ++i) assert(h2_pal_task_join(native_task, threads[i]) == 0);
    assert(f.s.writes == 7u && f.s.drains == 6u && f.router.records == 6u);
    assert(finish(&f, 0, 0u) == H2_PAL_ERR_TIMEOUT && f.s.confirms == 0u);
    close_fixture(&f);
}
static void timestamped_records(int usb) {
    fixture_t f = {0}; assert(prepare(&f, usb) == H2_PAL_OK);
    h2_pal_time_api_t clock = {.user = &f.s, .vtable = &clock_vtable};
    assert(h2_esp_e2e_result_log_use_clock(&f.router, &clock) == H2_PAL_OK);
    uint64_t before, after, observed;
    assert(h2_pal_time_get_monotonic_ms(native_time, &before) == H2_PAL_OK);
    assert(emit(&f, "CASE {}\n") == H2_PAL_OK);
    assert(h2_pal_time_get_monotonic_ms(native_time, &after) == H2_PAL_OK);
    int end = 0;
    assert(sscanf(f.s.bytes, "H2_E2E_RESULT source=pal_monotonic_ms time_ms=%" SCNu64 " CASE {}%n", &observed, &end) == 1);
    assert(before <= observed && observed <= after && strcmp(f.s.bytes + end, "\r\n") == 0);
    assert(f.s.writes == f.s.drains && f.s.writes == 1u && f.s.clock_reads == 1u);
    assert(h2_pal_log_write(&f.router.log, H2_PAL_LOG_WARN, "provider", "original log") == H2_PAL_OK);
    assert(f.s.clock_reads == 1u && f.s.delegated == 1u);
    assert(finish(&f, H2_PAL_OK, 1u) == H2_PAL_OK);
    size_t delivered = f.s.length;
    f.s.clock_error = H2_PAL_ERR_UNAVAILABLE;
    assert(emit(&f, "no clock fallback") == H2_PAL_ERR_UNAVAILABLE);
    assert(f.s.writes == 1u && f.s.length == delivered && f.s.clock_reads == 2u);
    f.s.clock_error = 0;
    assert(emit(&f, "later replay") == H2_PAL_ERR_UNAVAILABLE && f.s.clock_reads == 2u);
    assert(finish(&f, H2_PAL_OK, 1u) == H2_PAL_ERR_UNAVAILABLE && f.s.confirms == 1u);
    close_fixture(&f);
    fixture_t unavailable = {0}; assert(prepare(&unavailable, usb) == H2_PAL_OK);
    clock.user = &unavailable.s;
    unavailable.s.clock_error = H2_PAL_ERR_UNAVAILABLE;
    assert(h2_esp_e2e_result_log_use_clock(&unavailable.router, &clock) == H2_PAL_OK);
    assert(emit(&unavailable, "first clock failure") == H2_PAL_ERR_UNAVAILABLE);
    assert(unavailable.s.writes == 0u && unavailable.s.length == 0u);
    assert(finish(&unavailable, H2_PAL_OK, 0u) == H2_PAL_ERR_UNAVAILABLE && unavailable.s.confirms == 0u);
    close_fixture(&unavailable);
    fixture_t late = {0}; assert(prepare(&late, usb) == H2_PAL_OK);
    clock.user = &late.s;
    assert(emit(&late, "original untimed format") == H2_PAL_OK);
    assert(h2_esp_e2e_result_log_use_clock(&late.router, &clock) == H2_PAL_ERR_INVALID_STATE);
    assert(finish(&late, H2_PAL_OK, 0u) == H2_PAL_ERR_INVALID_STATE && late.s.confirms == 0u);
    close_fixture(&late);
    fixture_t missing = {0}; assert(prepare(&missing, usb) == H2_PAL_OK);
    const h2_pal_time_api_t no_clock = {0};
    assert(h2_esp_e2e_result_log_use_clock(&missing.router, &no_clock) == H2_PAL_ERR_UNSUPPORTED);
    assert(finish(&missing, H2_PAL_OK, 0u) == H2_PAL_ERR_UNSUPPORTED && missing.s.writes == 0u);
    close_fixture(&missing);
    fixture_t bounded = {0}; assert(prepare(&bounded, usb) == H2_PAL_OK);
    clock.user = &bounded.s;
    assert(h2_esp_e2e_result_log_use_clock(&bounded.router, &clock) == H2_PAL_OK);
    char body[1023]; memset(body, 'a', sizeof(body)-1u); body[sizeof(body)-1u] = '\0';
    assert(emit(&bounded, body) == H2_PAL_ERR_TRUNCATED && bounded.s.writes == 0u);
    assert(finish(&bounded, H2_PAL_OK, 0u) == H2_PAL_ERR_TRUNCATED && bounded.s.confirms == 0u);
    close_fixture(&bounded);
}
int main(void) {
#if defined(_WIN32)
    h2_windows_platform_t *platform = NULL;
    const h2_windows_platform_config_t config = {0};
    assert(h2_windows_platform_create(&config, &platform) == H2_PAL_OK);
    native_sync = h2_windows_sync_api(platform);
    native_task = h2_windows_task_api(platform);
    native_time = h2_windows_time_api(platform);
#else
    native_sync = h2_desktop_platform_sync_api();
    native_task = h2_desktop_platform_task_api();
    native_time = h2_desktop_platform_time_api();
#endif
    for (int usb = 0; usb <= 1; ++usb) { framing_and_delegate(usb); failures(usb); }
    for (int usb = 0; usb <= 1; ++usb) timestamped_records(usb);
    concurrent_records();
    concurrent_failure();
    bounds_and_capabilities();
    fixture_t create_failure = {0}; create_failure.s.create_error = H2_PAL_ERR_NO_MEMORY;
    assert(prepare(&create_failure, 0) == H2_PAL_ERR_NO_MEMORY);
    assert(create_failure.router.error.storage == NULL && create_failure.router.mutex == NULL);
    close_fixture(&create_failure);
    fixture_t absent_mutex = {0}; absent_mutex.s.null_mutex = 1;
    assert(prepare(&absent_mutex, 0) == H2_PAL_ERR_INVALID_STATE);
    assert(absent_mutex.router.error.storage == NULL && absent_mutex.router.mutex == NULL);
    close_fixture(&absent_mutex);
    for (unsigned kind = 0u; kind < 2u; ++kind) {
        fixture_t f = {0}; assert(prepare(&f, 0) == 0);
        if (kind == 0u) f.s.fail_lock = 1u;
        else f.s.fail_unlock = 1u;
        int expected = kind == 0u ? H2_PAL_ERR_TIMEOUT : H2_PAL_ERR_IO;
        assert(emit(&f, "record") == expected);
        assert(finish(&f, 0, 0u) == expected && f.s.confirms == 0u);
        close_fixture(&f);
    }
    fixture_t first_error = {0}; assert(prepare(&first_error, 0) == 0);
    first_error.s.fail_write = first_error.s.fail_unlock = 1u;
    assert(emit(&first_error, "record") == H2_PAL_ERR_TIMEOUT);
    assert(finish(&first_error, 0, 0u) == H2_PAL_ERR_TIMEOUT);
    close_fixture(&first_error);
    assert(h2_esp_e2e_result_log_confirm(NULL, 0, 1u, confirm, NULL) == H2_PAL_ERR_INVALID_STATE);
#if defined(_WIN32)
    assert(h2_windows_platform_destroy(&platform) == H2_PAL_OK);
#endif
    return 0;
}
