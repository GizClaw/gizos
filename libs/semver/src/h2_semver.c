#include "h2_semver.h"

#include <stddef.h>
#include <string.h>

typedef struct {
    const char *data;
    size_t size;
    bool numeric;
} identifier_t;

typedef struct {
    identifier_t core[3];
    const char *prerelease;
} version_t;

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

static bool is_identifier_char(char c) {
    return is_digit(c) || (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') || c == '-';
}

static identifier_t read_identifier(const char **cursor) {
    identifier_t id = {*cursor, 0, true};
    while (is_identifier_char(**cursor)) {
        if (!is_digit(**cursor)) {
            id.numeric = false;
        }
        ++id.size;
        ++*cursor;
    }
    return id;
}

static bool has_leading_zero(identifier_t id) {
    return id.numeric && id.size > 1 && id.data[0] == '0';
}

static bool parse_identifiers(const char **cursor, bool prerelease) {
    for (;;) {
        identifier_t id = read_identifier(cursor);
        if (id.size == 0 || (prerelease && has_leading_zero(id))) {
            return false;
        }
        if (**cursor != '.') {
            return true;
        }
        ++*cursor;
    }
}

static bool parse(const char *text, version_t *version) {
    if (text == NULL) {
        return false;
    }
    const char *cursor = text;
    for (size_t i = 0; i < 3; ++i) {
        identifier_t id = {cursor, 0, true};
        while (is_digit(*cursor)) {
            ++id.size;
            ++cursor;
        }
        if (id.size == 0 || has_leading_zero(id)) {
            return false;
        }
        version->core[i] = id;
        if (i < 2) {
            if (*cursor != '.') {
                return false;
            }
            ++cursor;
        }
    }
    version->prerelease = NULL;
    if (*cursor == '-') {
        version->prerelease = ++cursor;
        if (!parse_identifiers(&cursor, true)) {
            return false;
        }
    }
    if (*cursor == '+') {
        ++cursor;
        if (!parse_identifiers(&cursor, false)) {
            return false;
        }
    }
    return *cursor == '\0';
}

static int compare_identifier(identifier_t lhs, identifier_t rhs) {
    if (lhs.numeric != rhs.numeric) {
        return lhs.numeric ? -1 : 1;
    }
    /* Valid numeric identifiers have no leading zeros: length gives magnitude. */
    if (lhs.numeric && lhs.size != rhs.size) {
        return lhs.size < rhs.size ? -1 : 1;
    }
    size_t size = lhs.size < rhs.size ? lhs.size : rhs.size;
    int order = memcmp(lhs.data, rhs.data, size);
    if (order != 0) {
        return order < 0 ? -1 : 1;
    }
    return (lhs.size > rhs.size) - (lhs.size < rhs.size);
}

static int compare_versions(const version_t *lhs, const version_t *rhs) {
    for (size_t i = 0; i < 3; ++i) {
        int order = compare_identifier(lhs->core[i], rhs->core[i]);
        if (order != 0) {
            return order;
        }
    }
    const char *left = lhs->prerelease;
    const char *right = rhs->prerelease;
    if (left == NULL || right == NULL) {
        return (left == NULL) - (right == NULL);
    }
    for (;;) {
        identifier_t a = read_identifier(&left);
        identifier_t b = read_identifier(&right);
        int order = compare_identifier(a, b);
        if (order != 0) {
            return order;
        }
        bool more_left = *left == '.';
        bool more_right = *right == '.';
        if (!more_left || !more_right) {
            return (int)more_left - (int)more_right;
        }
        ++left;
        ++right;
    }
}

bool h2_semver_is_valid(const char *version) {
    version_t parsed;
    return parse(version, &parsed);
}

bool h2_semver_compare(const char *lhs, const char *rhs, int *out_result) {
    version_t left;
    version_t right;
    if (out_result == NULL) {
        return false;
    }
    bool left_valid = parse(lhs, &left);
    bool right_valid = parse(rhs, &right);
    *out_result = left_valid && right_valid
                  ? compare_versions(&left, &right)
                  : (int)left_valid - (int)right_valid;
    return true;
}
