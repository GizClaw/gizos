#include "h2_command_internal.h"

#include <string.h>

int h2_command_is_ascii_space(char value) {
    return value == ' ' || value == '\t' || value == '\v' || value == '\f';
}

h2_pal_result_t h2_command_parse_line(
    char *line,
    const char **argv,
    size_t argv_capacity,
    size_t *out_argc) {
    char *cursor;
    size_t argc = 0u;

    if (line == NULL || argv == NULL || argv_capacity == 0u || out_argc == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_argc = 0u;
    cursor = line;
    while (*cursor != '\0') {
        while (h2_command_is_ascii_space(*cursor)) {
            *cursor++ = '\0';
        }
        if (*cursor == '\0') {
            break;
        }
        if (argc == argv_capacity) {
            return H2_PAL_ERR_FULL;
        }
        argv[argc++] = cursor;
        while (*cursor != '\0' && !h2_command_is_ascii_space(*cursor)) {
            ++cursor;
        }
    }
    *out_argc = argc;
    return H2_PAL_OK;
}

size_t h2_command_path_token_count(const char *path) {
    size_t count = 0u;
    int in_token = 0;

    if (path == NULL) {
        return 0u;
    }
    for (; *path != '\0'; ++path) {
        if (h2_command_is_ascii_space(*path)) {
            in_token = 0;
        } else if (!in_token) {
            ++count;
            in_token = 1;
        }
    }
    return count;
}

int h2_command_path_matches(
    const char *path,
    size_t argc,
    const char *const *argv,
    size_t *out_tokens) {
    size_t token = 0u;
    const char *cursor = path;

    if (path == NULL || argv == NULL || out_tokens == NULL) {
        return 0;
    }
    while (*cursor != '\0') {
        const char *start;
        size_t len;

        while (h2_command_is_ascii_space(*cursor)) {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }
        start = cursor;
        while (*cursor != '\0' && !h2_command_is_ascii_space(*cursor)) {
            ++cursor;
        }
        len = (size_t)(cursor - start);
        if (token >= argc || strlen(argv[token]) != len || memcmp(argv[token], start, len) != 0) {
            return 0;
        }
        ++token;
    }
    *out_tokens = token;
    return token > 0u;
}

int h2_command_paths_equal(const char *left, const char *right) {
    const char *left_cursor = left;
    const char *right_cursor = right;

    if (left == NULL || right == NULL) {
        return 0;
    }
    for (;;) {
        const char *left_start;
        const char *right_start;
        size_t left_len;
        size_t right_len;

        while (h2_command_is_ascii_space(*left_cursor)) {
            ++left_cursor;
        }
        while (h2_command_is_ascii_space(*right_cursor)) {
            ++right_cursor;
        }
        if (*left_cursor == '\0' || *right_cursor == '\0') {
            return *left_cursor == '\0' && *right_cursor == '\0';
        }
        left_start = left_cursor;
        right_start = right_cursor;
        while (*left_cursor != '\0' && !h2_command_is_ascii_space(*left_cursor)) {
            ++left_cursor;
        }
        while (*right_cursor != '\0' && !h2_command_is_ascii_space(*right_cursor)) {
            ++right_cursor;
        }
        left_len = (size_t)(left_cursor - left_start);
        right_len = (size_t)(right_cursor - right_start);
        if (left_len != right_len || memcmp(left_start, right_start, left_len) != 0) {
            return 0;
        }
    }
}
