#include "h2_quectel_internal.h"

#include <string.h>

/* EC25 QECCNUM has two independent module tables, at most 20 entries each.
 * It does not identify entries from SIM files or a network emergency list. */
#define H2_QUECTEL_EMERGENCY_NUMBERS_PER_SCOPE 20u

static const char *skip_space(const char *cursor) {
    while (h2_quectel_ascii_space((unsigned char)*cursor)) { cursor++; }
    return cursor;
}

typedef struct emergency_query {
    h2_pal_modem_emergency_number_t *numbers;
    size_t capacity;
    size_t count;
    unsigned seen;
    h2_pal_result_t result;
} emergency_query_t;

static h2_pal_result_t parse_emergency_line(emergency_query_t *query, const char *line) {
    if (strncmp(line, "+QECCNUM:", 9u) != 0) { return H2_PAL_ERR_FORMAT; }
    const char *cursor = skip_space(line + 9u);
    if (*cursor != '0' && *cursor != '1') { return H2_PAL_ERR_FORMAT; }
    unsigned type = (unsigned)(*cursor++ - '0');
    unsigned mask = 1u << type;
    if ((query->seen & mask) != 0u) { return H2_PAL_ERR_FORMAT; }
    query->seen |= mask;
    cursor = skip_space(cursor);
    size_t scope_count = 0u;
    while (*cursor != '\0') {
        if (*cursor++ != ',') { return H2_PAL_ERR_FORMAT; }
        cursor = skip_space(cursor);
        if (*cursor++ != '"') { return H2_PAL_ERR_FORMAT; }
        const char *number = cursor;
        while (h2_quectel_ascii_digit((unsigned char)*cursor)) { cursor++; }
        size_t length = (size_t)(cursor - number);
        if (length == 0u || *cursor != '"') { return H2_PAL_ERR_FORMAT; }
        if (length >= H2_PAL_MODEM_PHONE_NUMBER_MAX) { return H2_PAL_ERR_TRUNCATED; }
        cursor = skip_space(cursor + 1u);
        if (++scope_count > H2_QUECTEL_EMERGENCY_NUMBERS_PER_SCOPE) {
            return H2_PAL_ERR_FORMAT;
        }
        if (query->count >= query->capacity) { return H2_PAL_ERR_TRUNCATED; }
        h2_pal_modem_emergency_number_t *entry = &query->numbers[query->count];
        memset(entry, 0, sizeof(*entry));
        memcpy(entry->number, number, length);
        entry->scope = type == 0u ? H2_PAL_MODEM_EMERGENCY_SCOPE_WITHOUT_SIM
                                 : H2_PAL_MODEM_EMERGENCY_SCOPE_WITH_SIM;
        entry->source = H2_PAL_MODEM_EMERGENCY_SOURCE_MODULE;
        query->count++;
    }
    return H2_PAL_OK;
}

/* Keep draining after a parse failure so the next operation cannot consume
 * this command's tail. PAL clears any partial caller storage on failure. */
static void collect_emergency_line(void *user, const char *line) {
    emergency_query_t *query = user;
    if (query->result == H2_PAL_OK) {
        query->result = parse_emergency_line(query, line);
    }
}

h2_pal_result_t h2_quectel_ec25_emergency_numbers(
    h2_quectel_modem_t *modem, uint32_t timeout_ms,
    h2_pal_modem_emergency_number_t *out_numbers, size_t capacity,
    size_t *out_count) {
    h2_quectel_response_t response = {0};
    emergency_query_t query = {.numbers = out_numbers, .capacity = capacity};
    h2_pal_result_t rc = h2_quectel_at_collect(modem, "AT+QECCNUM?", &response, timeout_ms,
        collect_emergency_line, &query);
    if (response.truncated) { rc = rc == H2_PAL_OK ? H2_PAL_ERR_TRUNCATED : rc; }
    if (rc == H2_PAL_OK &&
        (h2_quectel_response_find(&response, "ERROR") != NULL ||
         h2_quectel_response_find(&response, "+CME ERROR:") != NULL ||
         h2_quectel_response_find(&response, "+CMS ERROR:") != NULL)) {
        rc = H2_PAL_ERR_IO;
    }
    if (rc == H2_PAL_OK) {
        rc = query.result;
        if (rc == H2_PAL_OK && query.seen != 3u) { rc = H2_PAL_ERR_TRUNCATED; }
    }
    if (rc == H2_PAL_OK) { *out_count = query.count; }
    return rc;
}
