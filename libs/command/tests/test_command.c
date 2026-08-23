#include "h2_command.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    exit(1); \
} } while (0)

typedef struct fake_io {
    uint8_t input[2048];
    size_t input_len;
    size_t input_pos;
    size_t max_read;
    h2_pal_result_t empty_read_result;
    uint8_t output[2048];
    size_t output_len;
    size_t max_write;
    int zero_write;
    h2_pal_result_t write_result;
    h2_pal_result_t flush_result;
    size_t read_count;
    size_t write_count;
    size_t flush_count;
} fake_io_t;

typedef struct command_fixture {
    h2_command_t command;
    h2_command_definition_t definitions[8];
    char input_buffer[128];
    const char *argv[8];
} command_fixture_t;

typedef struct handler_capture {
    size_t calls;
    size_t argc;
    const char *response;
    h2_pal_result_t result;
} handler_capture_t;

typedef struct argv_capture {
    size_t calls;
    size_t argc;
    char value[32];
} argv_capture_t;

static h2_pal_result_t fake_read(
    void *user,
    void *buffer,
    size_t len,
    size_t *out_read,
    uint32_t timeout_ms) {
    fake_io_t *io = (fake_io_t *)user;
    size_t available = io->input_len - io->input_pos;
    size_t count = available < len ? available : len;
    (void)timeout_ms;

    io->read_count++;
    if (io->max_read > 0u && count > io->max_read) {
        count = io->max_read;
    }
    if (count > 0u) {
        memcpy(buffer, io->input + io->input_pos, count);
        io->input_pos += count;
    }
    *out_read = count;
    return count > 0u ? H2_PAL_OK : io->empty_read_result;
}

static h2_pal_result_t fake_write(
    void *user,
    const void *buffer,
    size_t len,
    size_t *out_written,
    uint32_t timeout_ms) {
    fake_io_t *io = (fake_io_t *)user;
    size_t count = len;
    (void)timeout_ms;

    io->write_count++;
    *out_written = 0u;
    if (io->write_result != H2_PAL_OK) {
        return io->write_result;
    }
    if (io->zero_write) {
        return H2_PAL_OK;
    }
    if (io->max_write > 0u && count > io->max_write) {
        count = io->max_write;
    }
    CHECK(io->output_len + count <= sizeof(io->output));
    memcpy(io->output + io->output_len, buffer, count);
    io->output_len += count;
    *out_written = count;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_flush(void *user) {
    fake_io_t *io = (fake_io_t *)user;
    io->flush_count++;
    return io->flush_result;
}

static const h2_command_io_vtable_t fake_io_vtable = {
    .read = fake_read,
    .write = fake_write,
    .flush = fake_flush,
};

static void fake_io_init(fake_io_t *io, const void *input, size_t input_len) {
    memset(io, 0, sizeof(*io));
    CHECK(input_len <= sizeof(io->input));
    if (input_len > 0u) {
        memcpy(io->input, input, input_len);
    }
    io->input_len = input_len;
    io->empty_read_result = H2_PAL_ERR_TIMEOUT;
    io->write_result = H2_PAL_OK;
    io->flush_result = H2_PAL_OK;
}

static void fake_io_append(fake_io_t *io, const void *input, size_t input_len) {
    CHECK(io->input_len + input_len <= sizeof(io->input));
    memcpy(io->input + io->input_len, input, input_len);
    io->input_len += input_len;
}

static void fixture_init(command_fixture_t *fixture, fake_io_t *io) {
    h2_command_config_t config;

    memset(fixture, 0, sizeof(*fixture));
    memset(&config, 0, sizeof(config));
    config.io.user = io;
    config.io.vtable = &fake_io_vtable;
    config.definitions = fixture->definitions;
    config.definition_capacity = sizeof(fixture->definitions) / sizeof(fixture->definitions[0]);
    config.input_buffer = fixture->input_buffer;
    config.input_buffer_size = sizeof(fixture->input_buffer);
    config.argv = fixture->argv;
    config.argv_capacity = sizeof(fixture->argv) / sizeof(fixture->argv[0]);
    config.write_timeout_ms = 25u;
    CHECK(h2_command_init(&fixture->command, &config) == H2_PAL_OK);
}

static h2_pal_result_t capture_handler(
    void *user,
    h2_command_t *command,
    size_t argc,
    const char *const *argv) {
    handler_capture_t *capture = (handler_capture_t *)user;
    (void)argv;

    capture->calls++;
    capture->argc = argc;
    if (capture->response != NULL) {
        h2_pal_result_t result = h2_command_write(command, capture->response, strlen(capture->response));
        if (result != H2_PAL_OK) {
            return result;
        }
    }
    return capture->result;
}

static void register_handler(
    command_fixture_t *fixture,
    const char *path,
    handler_capture_t *capture) {
    h2_command_definition_t definition = {
        .path = path,
        .help = path,
        .handler = capture_handler,
        .user = capture,
    };
    CHECK(h2_command_register(&fixture->command, &definition) == H2_PAL_OK);
}

static void test_init_register_and_freeze(void) {
    fake_io_t io;
    command_fixture_t fixture;
    handler_capture_t capture = { .result = H2_PAL_OK };
    h2_command_definition_t duplicate = {
        .path = "  status  ",
        .handler = capture_handler,
        .user = &capture,
    };
    h2_command_definition_t later = {
        .path = "later",
        .handler = capture_handler,
        .user = &capture,
    };

    fake_io_init(&io, "status\n", 7u);
    fixture_init(&fixture, &io);
    register_handler(&fixture, "status", &capture);
    CHECK(h2_command_register(&fixture.command, &duplicate) == H2_PAL_ERR_FORMAT);
    CHECK(h2_command_poll(&fixture.command, 10u) == H2_PAL_OK);
    CHECK(capture.calls == 1u);
    CHECK(h2_command_register(&fixture.command, &later) == H2_PAL_ERR_INVALID_STATE);

    fixture_init(&fixture, &io);
    CHECK(fixture.command.definition_count == 0u);
    CHECK(fixture.command.running == 0);
}

static void test_invalid_config_and_registration_capacity(void) {
    fake_io_t io;
    command_fixture_t fixture;
    handler_capture_t capture = { .result = H2_PAL_OK };
    h2_command_config_t config;
    h2_command_definition_t invalid = {
        .path = " \t ",
        .handler = capture_handler,
        .user = &capture,
    };

    fake_io_init(&io, NULL, 0u);
    memset(&config, 0, sizeof(config));
    CHECK(h2_command_init(NULL, &config) == H2_PAL_ERR_INVALID_ARG);
    CHECK(h2_command_init(&fixture.command, &config) == H2_PAL_ERR_INVALID_ARG);
    fixture_init(&fixture, &io);
    CHECK(h2_command_register(&fixture.command, &invalid) == H2_PAL_ERR_INVALID_ARG);
    register_handler(&fixture, "one", &capture);
    register_handler(&fixture, "two", &capture);
    register_handler(&fixture, "three", &capture);
    register_handler(&fixture, "four", &capture);
    register_handler(&fixture, "five", &capture);
    register_handler(&fixture, "six", &capture);
    register_handler(&fixture, "seven", &capture);
    register_handler(&fixture, "eight", &capture);
    invalid.path = "nine";
    CHECK(h2_command_register(&fixture.command, &invalid) == H2_PAL_ERR_FULL);
}

static void test_explicit_alias(void) {
    fake_io_t io;
    command_fixture_t fixture;
    handler_capture_t capture = { .result = H2_PAL_OK };

    fake_io_init(&io, "status\nstats\n", 13u);
    fixture_init(&fixture, &io);
    register_handler(&fixture, "status", &capture);
    register_handler(&fixture, "stats", &capture);
    CHECK(h2_command_poll(&fixture.command, 10u) == H2_PAL_OK);
    CHECK(h2_command_poll(&fixture.command, 10u) == H2_PAL_OK);
    CHECK(capture.calls == 2u);
}

static void test_fragmented_input_and_partial_write(void) {
    fake_io_t io;
    command_fixture_t fixture;
    handler_capture_t capture = {
        .response = "response",
        .result = H2_PAL_OK,
    };

    fake_io_init(&io, "root child arg\n", 15u);
    io.max_read = 2u;
    io.max_write = 3u;
    fixture_init(&fixture, &io);
    register_handler(&fixture, "root child", &capture);
    CHECK(h2_command_poll(&fixture.command, 10u) == H2_PAL_OK);
    CHECK(capture.calls == 1u);
    CHECK(capture.argc == 3u);
    CHECK(io.write_count == 3u);
    CHECK(io.flush_count == 1u);
    CHECK(io.output_len == strlen("response"));
    CHECK(memcmp(io.output, "response", io.output_len) == 0);
}

static void test_one_command_per_poll_and_longest_path(void) {
    fake_io_t io;
    command_fixture_t fixture;
    handler_capture_t parent = { .response = "parent", .result = H2_PAL_OK };
    handler_capture_t child = { .response = "child", .result = H2_PAL_OK };

    fake_io_init(&io, "root child\nroot\n", 16u);
    fixture_init(&fixture, &io);
    register_handler(&fixture, "root", &parent);
    register_handler(&fixture, "root child", &child);
    CHECK(h2_command_poll(&fixture.command, 10u) == H2_PAL_OK);
    CHECK(child.calls == 1u);
    CHECK(parent.calls == 0u);
    CHECK(h2_command_poll(&fixture.command, 10u) == H2_PAL_OK);
    CHECK(parent.calls == 1u);
    CHECK(io.flush_count == 2u);
    CHECK(memcmp(io.output, "childparent", strlen("childparent")) == 0);
}

static h2_pal_result_t raw_handler(
    void *user,
    h2_command_t *command,
    size_t argc,
    const char *const *argv) {
    uint8_t *payload = (uint8_t *)user;
    (void)argc;
    (void)argv;
    return h2_command_read_exact(command, payload, 4u, 10u);
}

static h2_pal_result_t ignored_write_handler(
    void *user,
    h2_command_t *command,
    size_t argc,
    const char *const *argv) {
    static const char response[] = "response";

    (void)user;
    (void)argc;
    (void)argv;
    (void)h2_command_write(command, response, sizeof(response) - 1u);
    return H2_PAL_OK;
}

static h2_pal_result_t argv_capture_handler(
    void *user,
    h2_command_t *command,
    size_t argc,
    const char *const *argv) {
    argv_capture_t *capture = (argv_capture_t *)user;

    (void)command;
    capture->calls++;
    capture->argc = argc;
    CHECK(argc >= 3u);
    CHECK(strlen(argv[2]) < sizeof(capture->value));
    memcpy(capture->value, argv[2], strlen(argv[2]) + 1u);
    return H2_PAL_OK;
}

static void test_raw_payload_uses_prefetched_bytes(void) {
    static const char input[] = "stage 4\nDATAstatus\n";
    fake_io_t io;
    command_fixture_t fixture;
    uint8_t payload[4] = { 0u };
    handler_capture_t status = { .result = H2_PAL_OK };
    h2_command_definition_t stage = {
        .path = "stage",
        .handler = raw_handler,
        .user = payload,
    };

    fake_io_init(&io, input, sizeof(input) - 1u);
    fixture_init(&fixture, &io);
    CHECK(h2_command_register(&fixture.command, &stage) == H2_PAL_OK);
    register_handler(&fixture, "status", &status);
    CHECK(h2_command_poll(&fixture.command, 10u) == H2_PAL_OK);
    CHECK(memcmp(payload, "DATA", sizeof(payload)) == 0);
    CHECK(h2_command_poll(&fixture.command, 10u) == H2_PAL_OK);
    CHECK(status.calls == 1u);
}

static void test_crlf_raw_payload_skips_line_feed(void) {
    static const char input[] = "stage 4\r\nDATAstatus\r\n";
    fake_io_t io;
    command_fixture_t fixture;
    uint8_t payload[4] = { 0u };
    handler_capture_t status = { .result = H2_PAL_OK };
    h2_command_definition_t stage = {
        .path = "stage",
        .handler = raw_handler,
        .user = payload,
    };

    fake_io_init(&io, input, sizeof(input) - 1u);
    fixture_init(&fixture, &io);
    CHECK(h2_command_register(&fixture.command, &stage) == H2_PAL_OK);
    register_handler(&fixture, "status", &status);
    CHECK(h2_command_poll(&fixture.command, 10u) == H2_PAL_OK);
    CHECK(memcmp(payload, "DATA", sizeof(payload)) == 0);
    CHECK(h2_command_poll(&fixture.command, 10u) == H2_PAL_OK);
    CHECK(status.calls == 1u);
}

static void test_split_crlf_raw_payload_skips_line_feed(void) {
    static const char input[] = "stage 4\r\nDATAstatus\r\n";
    fake_io_t io;
    command_fixture_t fixture;
    uint8_t payload[4] = { 0u };
    handler_capture_t status = { .result = H2_PAL_OK };
    h2_command_definition_t stage = {
        .path = "stage",
        .handler = raw_handler,
        .user = payload,
    };

    fake_io_init(&io, input, sizeof(input) - 1u);
    io.max_read = 8u;
    fixture_init(&fixture, &io);
    CHECK(h2_command_register(&fixture.command, &stage) == H2_PAL_OK);
    register_handler(&fixture, "status", &status);
    CHECK(h2_command_poll(&fixture.command, 10u) == H2_PAL_OK);
    CHECK(memcmp(payload, "DATA", sizeof(payload)) == 0);
    CHECK(h2_command_poll(&fixture.command, 10u) == H2_PAL_OK);
    CHECK(status.calls == 1u);
}

static void test_carriage_return_terminates_command_after_lookahead_timeout(void) {
    fake_io_t io;
    command_fixture_t fixture;
    handler_capture_t capture = { .result = H2_PAL_OK };

    fake_io_init(&io, "status\r", 7u);
    fixture_init(&fixture, &io);
    register_handler(&fixture, "status", &capture);
    CHECK(h2_command_poll(&fixture.command, 10u) == H2_PAL_OK);
    CHECK(capture.calls == 1u);
    CHECK(io.read_count == 2u);
}

static void test_execute_preserves_tokenized_arguments(void) {
    static const char *const argv[] = { "wifi", "connect", "My Network" };
    fake_io_t io;
    command_fixture_t fixture;
    argv_capture_t capture = { 0 };
    h2_command_definition_t definition = {
        .path = "wifi connect",
        .handler = argv_capture_handler,
        .user = &capture,
    };
    h2_command_definition_t later = {
        .path = "later",
        .handler = argv_capture_handler,
        .user = &capture,
    };

    fake_io_init(&io, NULL, 0u);
    fixture_init(&fixture, &io);
    CHECK(h2_command_register(&fixture.command, &definition) == H2_PAL_OK);
    CHECK(h2_command_execute(&fixture.command, 3u, argv) == H2_PAL_OK);
    CHECK(capture.calls == 1u);
    CHECK(capture.argc == 3u);
    CHECK(strcmp(capture.value, "My Network") == 0);
    CHECK(io.flush_count == 1u);
    CHECK(h2_command_register(&fixture.command, &later) == H2_PAL_ERR_INVALID_STATE);
}

static void test_timeout_preserves_partial_line(void) {
    fake_io_t io;
    command_fixture_t fixture;
    handler_capture_t capture = { .result = H2_PAL_OK };

    fake_io_init(&io, "sta", 3u);
    fixture_init(&fixture, &io);
    register_handler(&fixture, "status", &capture);
    CHECK(h2_command_poll(&fixture.command, 10u) == H2_PAL_ERR_TIMEOUT);
    CHECK(capture.calls == 0u);
    fake_io_append(&io, "tus\n", 4u);
    CHECK(h2_command_poll(&fixture.command, 10u) == H2_PAL_OK);
    CHECK(capture.calls == 1u);
}

static void test_empty_unknown_and_handler_error(void) {
    fake_io_t io;
    command_fixture_t fixture;
    handler_capture_t capture = {
        .response = "error",
        .result = H2_PAL_ERR_FORMAT,
    };

    fake_io_init(&io, " \t\nunknown\nfail\n", 17u);
    fixture_init(&fixture, &io);
    register_handler(&fixture, "fail", &capture);
    CHECK(h2_command_poll(&fixture.command, 10u) == H2_PAL_ERR_NOT_FOUND);
    CHECK(io.flush_count == 0u);
    CHECK(h2_command_poll(&fixture.command, 10u) == H2_PAL_ERR_FORMAT);
    CHECK(capture.calls == 1u);
    CHECK(io.flush_count == 1u);
    CHECK(memcmp(io.output, "error", 5u) == 0);
}

static void test_line_and_argv_capacity_errors(void) {
    fake_io_t io;
    command_fixture_t fixture;
    handler_capture_t capture = { .result = H2_PAL_OK };
    uint8_t long_line[160];

    memset(long_line, 'x', sizeof(long_line));
    fake_io_init(&io, long_line, sizeof(long_line));
    fixture_init(&fixture, &io);
    register_handler(&fixture, "x", &capture);
    CHECK(h2_command_poll(&fixture.command, 10u) == H2_PAL_ERR_TRUNCATED);

    fake_io_init(&io, "a b c d e f g h i\n", 18u);
    fixture_init(&fixture, &io);
    register_handler(&fixture, "a", &capture);
    CHECK(h2_command_poll(&fixture.command, 10u) == H2_PAL_ERR_FULL);
}

static void test_io_failures(void) {
    fake_io_t io;
    command_fixture_t fixture;
    handler_capture_t capture = { .response = "response", .result = H2_PAL_OK };

    fake_io_init(&io, "status\n", 7u);
    io.write_result = H2_PAL_ERR_CLOSED;
    fixture_init(&fixture, &io);
    register_handler(&fixture, "status", &capture);
    CHECK(h2_command_poll(&fixture.command, 10u) == H2_PAL_ERR_CLOSED);

    fake_io_init(&io, "status\n", 7u);
    io.flush_result = H2_PAL_ERR_IO;
    fixture_init(&fixture, &io);
    register_handler(&fixture, "status", &capture);
    CHECK(h2_command_poll(&fixture.command, 10u) == H2_PAL_ERR_IO);

    fake_io_init(&io, "part", 4u);
    io.empty_read_result = H2_PAL_ERR_WOULD_BLOCK;
    fixture_init(&fixture, &io);
    register_handler(&fixture, "partial", &capture);
    CHECK(h2_command_poll(&fixture.command, 10u) == H2_PAL_ERR_WOULD_BLOCK);
    fake_io_append(&io, "ial\n", 4u);
    CHECK(h2_command_poll(&fixture.command, 10u) == H2_PAL_OK);

    fake_io_init(&io, NULL, 0u);
    io.empty_read_result = H2_PAL_ERR_CLOSED;
    fixture_init(&fixture, &io);
    register_handler(&fixture, "status", &capture);
    CHECK(h2_command_poll(&fixture.command, 10u) == H2_PAL_ERR_CLOSED);

    fake_io_init(&io, NULL, 0u);
    io.empty_read_result = H2_PAL_OK;
    fixture_init(&fixture, &io);
    register_handler(&fixture, "status", &capture);
    CHECK(h2_command_poll(&fixture.command, 10u) == H2_PAL_ERR_IO);

    fake_io_init(&io, "status\n", 7u);
    io.zero_write = 1;
    fixture_init(&fixture, &io);
    capture.response = "response";
    register_handler(&fixture, "status", &capture);
    CHECK(h2_command_poll(&fixture.command, 10u) == H2_PAL_ERR_IO);
}

static void test_ignored_handler_write_failure_is_propagated(void) {
    fake_io_t io;
    command_fixture_t fixture;
    h2_command_definition_t definition = {
        .path = "status",
        .handler = ignored_write_handler,
    };

    fake_io_init(&io, "status\n", 7u);
    io.write_result = H2_PAL_ERR_CLOSED;
    fixture_init(&fixture, &io);
    CHECK(h2_command_register(&fixture.command, &definition) == H2_PAL_OK);
    CHECK(h2_command_poll(&fixture.command, 10u) == H2_PAL_ERR_CLOSED);
    CHECK(io.flush_count == 1u);
}

int main(void) {
    test_init_register_and_freeze();
    test_invalid_config_and_registration_capacity();
    test_explicit_alias();
    test_fragmented_input_and_partial_write();
    test_one_command_per_poll_and_longest_path();
    test_raw_payload_uses_prefetched_bytes();
    test_crlf_raw_payload_skips_line_feed();
    test_split_crlf_raw_payload_skips_line_feed();
    test_carriage_return_terminates_command_after_lookahead_timeout();
    test_execute_preserves_tokenized_arguments();
    test_timeout_preserves_partial_line();
    test_empty_unknown_and_handler_error();
    test_line_and_argv_capacity_errors();
    test_io_failures();
    test_ignored_handler_write_failure_is_propagated();
    puts("command tests passed");
    return 0;
}
