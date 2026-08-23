#ifndef H2_COMMAND_H
#define H2_COMMAND_H

#include "h2/pal/core/h2_pal_errors.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_command h2_command_t;

/** @brief Synchronous byte-stream operations borrowed by a command instance. */
typedef struct h2_command_io_vtable {
    h2_pal_result_t (*read)(
        void *user,
        void *buffer,
        size_t len,
        size_t *out_read,
        uint32_t timeout_ms);
    h2_pal_result_t (*write)(
        void *user,
        const void *buffer,
        size_t len,
        size_t *out_written,
        uint32_t timeout_ms);
    h2_pal_result_t (*flush)(void *user);
} h2_command_io_vtable_t;

/** @brief Borrowed synchronous I/O backend used by one command stream. */
typedef struct h2_command_io_api {
    void *user;
    const h2_command_io_vtable_t *vtable;
} h2_command_io_api_t;

/**
 * @brief Synchronous command handler.
 *
 * The command, argv array, and strings are borrowed for the duration of the
 * call. A handler may synchronously read payload bytes and write its response
 * through the command helpers, but it must not retain any borrowed pointer.
 */
typedef h2_pal_result_t (*h2_command_handler_fn)(
    void *user,
    h2_command_t *command,
    size_t argc,
    const char *const *argv);

/** @brief Registration copied into caller-provided definition storage. */
typedef struct h2_command_definition {
    const char *path;
    const char *help;
    h2_command_handler_fn handler;
    void *user;
} h2_command_definition_t;

/** @brief Caller-owned storage and borrowed I/O used to initialize a command. */
typedef struct h2_command_config {
    h2_command_io_api_t io;
    h2_command_definition_t *definitions;
    size_t definition_capacity;
    char *input_buffer;
    size_t input_buffer_size;
    const char **argv;
    size_t argv_capacity;
    uint32_t write_timeout_ms;
} h2_command_config_t;

/**
 * @brief Caller-owned state for one synchronous command stream.
 *
 * Fields are public only to allow static or stack allocation. Consumers must
 * treat them as private and access the instance through this API.
 */
struct h2_command {
    h2_command_io_api_t io;
    h2_command_definition_t *definitions;
    size_t definition_count;
    size_t definition_capacity;
    char *input_buffer;
    size_t input_buffer_size;
    size_t input_len;
    size_t pending_offset;
    const char **argv;
    size_t argv_capacity;
    uint32_t write_timeout_ms;
    h2_pal_result_t response_result;
    int initialized;
    int running;
    int executing;
};

/**
 * @brief Initialize or reset a caller-owned command instance.
 * @return H2_PAL_OK, or an invalid-argument result.
 */
h2_pal_result_t h2_command_init(
    h2_command_t *command,
    const h2_command_config_t *config);

/**
 * @brief Register a borrowed path and handler before the first execution.
 *
 * The registry freezes after the first h2_command_execute() or
 * h2_command_poll() call.
 * @return H2_PAL_OK, invalid argument/state, full, or duplicate format error.
 */
h2_pal_result_t h2_command_register(
    h2_command_t *command,
    const h2_command_definition_t *definition);

/**
 * @brief Synchronously execute one caller-tokenized command.
 *
 * The argv array and strings are borrowed for the duration of the call. This
 * entry preserves argument boundaries supplied by an existing command parser.
 */
h2_pal_result_t h2_command_execute(
    h2_command_t *command,
    size_t argc,
    const char *const *argv);

/**
 * @brief Read and synchronously execute at most one complete command.
 *
 * Command lines may end with LF, CR, or CRLF. A trailing CR is accepted as a
 * complete terminator when the lookahead read times out.
 *
 * Partial input is retained after timeout or would-block. A completed handler
 * response is flushed before this function returns. Handler errors take
 * precedence over response write errors, which take precedence over the final
 * flush result.
 */
h2_pal_result_t h2_command_poll(
    h2_command_t *command,
    uint32_t timeout_ms);

/** @brief Read up to len payload bytes from the current command stream. */
h2_pal_result_t h2_command_read(
    h2_command_t *command,
    void *buffer,
    size_t len,
    size_t *out_read,
    uint32_t timeout_ms);

/** @brief Read exactly len payload bytes or return the first stream error. */
h2_pal_result_t h2_command_read_exact(
    h2_command_t *command,
    void *buffer,
    size_t len,
    uint32_t timeout_ms);

/** @brief Write all response bytes or return the first stream error. */
h2_pal_result_t h2_command_write(
    h2_command_t *command,
    const void *buffer,
    size_t len);

/** @brief Flush response bytes through the bound stream backend. */
h2_pal_result_t h2_command_flush(h2_command_t *command);

#ifdef __cplusplus
}
#endif

#endif
