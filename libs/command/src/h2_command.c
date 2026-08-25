#include "h2_command.h"

#include "h2_command_internal.h"

#include <string.h>

static int h2_command_io_valid(const h2_command_io_api_t *io) {
    return io != NULL && io->vtable != NULL && io->vtable->read != NULL &&
           io->vtable->write != NULL && io->vtable->flush != NULL;
}

static void h2_command_compact_input(h2_command_t *command) {
    size_t remaining;

    if (command->pending_offset == 0u) {
        return;
    }
    if (command->pending_offset >= command->input_len) {
        command->input_len = 0u;
        command->pending_offset = 0u;
        return;
    }
    remaining = command->input_len - command->pending_offset;
    memmove(command->input_buffer, command->input_buffer + command->pending_offset, remaining);
    command->input_len = remaining;
    command->pending_offset = 0u;
}

static int h2_command_find_line(const h2_command_t *command, size_t *out_end) {
    size_t index;

    for (index = command->pending_offset; index < command->input_len; ++index) {
        char value = command->input_buffer[index];
        if (value == '\0') {
            return -1;
        }
        if (value == '\r' || value == '\n') {
            *out_end = index;
            return 1;
        }
    }
    return 0;
}

static h2_pal_result_t h2_command_read_more(h2_command_t *command, uint32_t timeout_ms) {
    size_t available;
    size_t out_read = 0u;
    h2_pal_result_t result;

    h2_command_compact_input(command);
    if (command->input_len + 1u >= command->input_buffer_size) {
        command->input_len = 0u;
        return H2_PAL_ERR_TRUNCATED;
    }
    available = command->input_buffer_size - command->input_len - 1u;
    result = command->io.vtable->read(
        command->io.user,
        command->input_buffer + command->input_len,
        available,
        &out_read,
        timeout_ms);
    if (out_read > available) {
        command->input_len = 0u;
        return H2_PAL_ERR_IO;
    }
    command->input_len += out_read;
    command->input_buffer[command->input_len] = '\0';
    if (result != H2_PAL_OK) {
        return result;
    }
    return out_read == 0u ? H2_PAL_ERR_IO : H2_PAL_OK;
}

static h2_pal_result_t h2_command_route_marker(
    const void *user,
    const h2_trie_match_t *match,
    void *response) {
    (void)user;
    (void)match;
    (void)response;
    return H2_PAL_ERR_INVALID_STATE;
}

static const h2_command_definition_t *h2_command_find_definition(
    const h2_command_t *command,
    size_t argc,
    const char *const *argv) {
    const h2_trie_route_t *route = NULL;

    if (h2_trie_find_tokens(&command->router, argc, argv, &route) != H2_PAL_OK) {
        return NULL;
    }
    return (const h2_command_definition_t *)route->user;
}

h2_pal_result_t h2_command_init(
    h2_command_t *command,
    const h2_command_config_t *config) {
    if (command == NULL || config == NULL || !h2_command_io_valid(&config->io) ||
        config->definitions == NULL || config->definition_capacity == 0u ||
        config->routes == NULL || config->route_nodes == NULL ||
        config->route_node_capacity == 0u ||
        config->input_buffer == NULL || config->input_buffer_size < 2u ||
        config->argv == NULL || config->argv_capacity == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(command, 0, sizeof(*command));
    command->io = config->io;
    command->definitions = config->definitions;
    command->definition_capacity = config->definition_capacity;
    command->routes = config->routes;
    command->route_nodes = config->route_nodes;
    command->route_node_capacity = config->route_node_capacity;
    command->input_buffer = config->input_buffer;
    command->input_buffer_size = config->input_buffer_size;
    command->argv = config->argv;
    command->argv_capacity = config->argv_capacity;
    command->write_timeout_ms = config->write_timeout_ms;
    command->input_buffer[0] = '\0';
    if (h2_trie_build_tokens(
            &command->router,
            command->route_nodes,
            command->route_node_capacity,
            command->routes,
            0u) != H2_PAL_OK) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    command->initialized = 1;
    return H2_PAL_OK;
}

h2_pal_result_t h2_command_register(
    h2_command_t *command,
    const h2_command_definition_t *definition) {
    size_t path_tokens;
    size_t index;
    h2_pal_result_t result;

    if (command == NULL || definition == NULL || definition->path == NULL ||
        definition->handler == NULL || !command->initialized) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (command->running || command->executing) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    path_tokens = h2_command_path_token_count(definition->path);
    if (path_tokens == 0u || path_tokens > command->argv_capacity) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    for (index = 0u; index < command->definition_count; ++index) {
        if (h2_command_paths_equal(command->definitions[index].path, definition->path)) {
            return H2_PAL_ERR_FORMAT;
        }
    }
    if (command->definition_count == command->definition_capacity) {
        return H2_PAL_ERR_FULL;
    }
    index = command->definition_count;
    command->definitions[index] = *definition;
    command->routes[index].path = definition->path;
    command->routes[index].mode = H2_TRIE_ROUTE_EXACT_OR_PREFIX;
    command->routes[index].handler = h2_command_route_marker;
    command->routes[index].user = &command->definitions[index];
    command->definition_count++;
    result = h2_trie_build_tokens(
        &command->router,
        command->route_nodes,
        command->route_node_capacity,
        command->routes,
        command->definition_count);
    if (result != H2_PAL_OK) {
        command->definition_count--;
        (void)h2_trie_build_tokens(
            &command->router,
            command->route_nodes,
            command->route_node_capacity,
            command->routes,
            command->definition_count);
        return result;
    }
    return H2_PAL_OK;
}

h2_pal_result_t h2_command_execute(
    h2_command_t *command,
    size_t argc,
    const char *const *argv) {
    const h2_command_definition_t *definition;
    h2_pal_result_t result;
    h2_pal_result_t flush_result;

    if (command == NULL || !command->initialized || argc == 0u || argv == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (argc > command->argv_capacity) {
        return H2_PAL_ERR_FULL;
    }
    if (command->executing) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    for (size_t index = 0u; index < argc; ++index) {
        if (argv[index] == NULL) {
            return H2_PAL_ERR_INVALID_ARG;
        }
    }
    command->running = 1;
    definition = h2_command_find_definition(command, argc, argv);
    if (definition == NULL) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    command->response_result = H2_PAL_OK;
    command->executing = 1;
    result = definition->handler(definition->user, command, argc, argv);
    command->executing = 0;
    flush_result = h2_command_flush(command);
    if (result != H2_PAL_OK) {
        return result;
    }
    if (command->response_result != H2_PAL_OK) {
        return command->response_result;
    }
    return flush_result;
}

h2_pal_result_t h2_command_poll(
    h2_command_t *command,
    uint32_t timeout_ms) {
    h2_pal_result_t result;

    if (command == NULL || !command->initialized) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (command->executing) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    command->running = 1;
    for (;;) {
        size_t line_end = 0u;
        size_t argc = 0u;
        int line_state = h2_command_find_line(command, &line_end);

        if (line_state < 0) {
            command->input_len = 0u;
            command->pending_offset = 0u;
            return H2_PAL_ERR_FORMAT;
        }
        if (line_state == 0) {
            result = h2_command_read_more(command, timeout_ms);
            if (result != H2_PAL_OK) {
                return result;
            }
            continue;
        }
        if (command->input_buffer[line_end] == '\r' && line_end + 1u == command->input_len) {
            result = h2_command_read_more(command, timeout_ms);
            if (result != H2_PAL_OK && result != H2_PAL_ERR_TIMEOUT) {
                return result;
            }
            if (result == H2_PAL_OK) {
                continue;
            }
        }
        {
            char delimiter = command->input_buffer[line_end];

            command->input_buffer[line_end] = '\0';
            command->pending_offset = line_end + 1u;
            if (delimiter == '\r' &&
                command->pending_offset < command->input_len &&
                command->input_buffer[command->pending_offset] == '\n') {
                command->pending_offset++;
            }
        }
        result = h2_command_parse_line(
            command->input_buffer,
            command->argv,
            command->argv_capacity,
            &argc);
        if (result != H2_PAL_OK) {
            h2_command_compact_input(command);
            return result;
        }
        if (argc == 0u) {
            h2_command_compact_input(command);
            continue;
        }
        result = h2_command_execute(command, argc, command->argv);
        h2_command_compact_input(command);
        return result;
    }
}

h2_pal_result_t h2_command_read(
    h2_command_t *command,
    void *buffer,
    size_t len,
    size_t *out_read,
    uint32_t timeout_ms) {
    size_t available;

    if (command == NULL || !command->initialized || buffer == NULL || out_read == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_read = 0u;
    if (len == 0u) {
        return H2_PAL_OK;
    }
    available = command->input_len - command->pending_offset;
    if (available > 0u) {
        size_t count = available < len ? available : len;
        memcpy(buffer, command->input_buffer + command->pending_offset, count);
        command->pending_offset += count;
        *out_read = count;
        return H2_PAL_OK;
    }
    {
        h2_pal_result_t result = command->io.vtable->read(
            command->io.user,
            buffer,
            len,
            out_read,
            timeout_ms);
        if (*out_read > len) {
            *out_read = 0u;
            return H2_PAL_ERR_IO;
        }
        return result;
    }
}

h2_pal_result_t h2_command_read_exact(
    h2_command_t *command,
    void *buffer,
    size_t len,
    uint32_t timeout_ms) {
    size_t offset = 0u;

    if (command == NULL || (buffer == NULL && len != 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    while (offset < len) {
        size_t out_read = 0u;
        h2_pal_result_t result = h2_command_read(
            command,
            (uint8_t *)buffer + offset,
            len - offset,
            &out_read,
            timeout_ms);
        if (result != H2_PAL_OK) {
            return result;
        }
        if (out_read == 0u || out_read > len - offset) {
            return H2_PAL_ERR_IO;
        }
        offset += out_read;
    }
    return H2_PAL_OK;
}

h2_pal_result_t h2_command_write(
    h2_command_t *command,
    const void *buffer,
    size_t len) {
    size_t offset = 0u;

    if (command == NULL || !command->initialized || (buffer == NULL && len != 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (command->executing && command->response_result != H2_PAL_OK) {
        return command->response_result;
    }
    while (offset < len) {
        size_t out_written = 0u;
        h2_pal_result_t result = command->io.vtable->write(
            command->io.user,
            (const uint8_t *)buffer + offset,
            len - offset,
            &out_written,
            command->write_timeout_ms);
        if (result != H2_PAL_OK) {
            if (command->executing) {
                command->response_result = result;
            }
            return result;
        }
        if (out_written == 0u || out_written > len - offset) {
            if (command->executing) {
                command->response_result = H2_PAL_ERR_IO;
            }
            return H2_PAL_ERR_IO;
        }
        offset += out_written;
    }
    return H2_PAL_OK;
}

h2_pal_result_t h2_command_flush(h2_command_t *command) {
    if (command == NULL || !command->initialized) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return command->io.vtable->flush(command->io.user);
}
