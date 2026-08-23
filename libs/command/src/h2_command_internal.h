#ifndef H2_COMMAND_INTERNAL_H
#define H2_COMMAND_INTERNAL_H

#include "h2_command.h"

int h2_command_is_ascii_space(char value);
h2_pal_result_t h2_command_parse_line(
    char *line,
    const char **argv,
    size_t argv_capacity,
    size_t *out_argc);
size_t h2_command_path_token_count(const char *path);
int h2_command_path_matches(
    const char *path,
    size_t argc,
    const char *const *argv,
    size_t *out_tokens);
int h2_command_paths_equal(const char *left, const char *right);

#endif
