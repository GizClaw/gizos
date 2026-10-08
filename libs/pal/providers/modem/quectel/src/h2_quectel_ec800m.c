#include "h2_quectel_internal.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PHONEBOOK_RANGES_MAX 16u
#define PHONEBOOK_PAGE_SIZE 8u
/* Bound the number of AT transactions for a corrupt/huge advertised index
 * space. This is a provider scan limit, not a claim about module capacity. */
#define PHONEBOOK_SCAN_MAX 4096u

static const char *skip_space(const char *cursor) {
    while (h2_quectel_ascii_space((unsigned char)*cursor)) { cursor++; }
    return cursor;
}

static int parse_uint(const char **cursor, uint32_t *out) {
    const char *p = skip_space(*cursor);
    uint32_t value = 0u;
    if (!h2_quectel_ascii_digit((unsigned char)*p)) { return 0; }
    while (h2_quectel_ascii_digit((unsigned char)*p)) {
        unsigned digit = (unsigned)(*p++ - '0');
        if (value > (UINT32_MAX - digit) / 10u) { return 0; }
        value = value * 10u + digit;
    }
    *out = value;
    *cursor = skip_space(p);
    return 1;
}

static int take_comma(const char **cursor) {
    if (**cursor != ',') { return 0; }
    *cursor = skip_space(*cursor + 1u);
    return 1;
}

static int parse_storage(const char **cursor, char *out, size_t capacity) {
    const char *p = skip_space(*cursor);
    if (*p++ != '"') { return 0; }
    size_t length = 0u;
    while ((*p >= 'A' && *p <= 'Z') || h2_quectel_ascii_digit((unsigned char)*p)) {
        if (length + 1u >= capacity) { return 0; }
        out[length++] = *p++;
    }
    if (length == 0u || *p != '"') { return 0; }
    out[length] = '\0';
    *cursor = skip_space(p + 1u);
    return 1;
}

/* A bare ERROR proves lack of support only for the capability-test form.
 * Availability, transport and cleanup failures must remain distinct. */
static h2_pal_result_t exchange(h2_quectel_modem_t *modem, const char *command,
    uint32_t timeout_ms, h2_quectel_response_t *response, int probe,
    h2_quectel_at_line_fn collect, void *user) {
    memset(response, 0, sizeof(*response));
    const uint32_t reset_generation = modem->reset_generation;
    const uint32_t sim_generation = modem->sim_generation;
    h2_pal_result_t rc = collect != NULL
        ? h2_quectel_at_collect(modem, command, response, timeout_ms, collect, user)
        : h2_quectel_at_exchange_timeout(modem, command, response, 0, timeout_ms);
    if (reset_generation != modem->reset_generation || sim_generation != modem->sim_generation) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    const char *error = h2_quectel_response_find(response, "+CME ERROR:");
    if (error != NULL) {
        int code = 0;
        if (h2_quectel_parse_int_after(error, "+CME ERROR:", &code)) {
            if (code == 4) { return H2_PAL_ERR_UNSUPPORTED; }
            if (code == 10) { return H2_PAL_ERR_UNAVAILABLE; }
            if (code == 3 || code == 11 || code == 12) { return H2_PAL_ERR_INVALID_STATE; }
        }
        if (strstr(error, "operation not supported") != NULL) { return H2_PAL_ERR_UNSUPPORTED; }
        if (strstr(error, "SIM not inserted") != NULL) { return H2_PAL_ERR_UNAVAILABLE; }
        return rc == H2_PAL_OK ? H2_PAL_ERR_IO : rc;
    }
    if (h2_quectel_response_find(response, "ERROR") != NULL) {
        return probe ? H2_PAL_ERR_UNSUPPORTED : H2_PAL_ERR_IO;
    }
    if (rc == H2_PAL_OK && response->truncated) { return H2_PAL_ERR_TRUNCATED; }
    return rc;
}

static h2_pal_result_t supports_emergency_phonebook(const h2_quectel_response_t *response) {
    if (response->count != 1u || strncmp(response->lines[0], "+CPBS:", 6u) != 0) {
        return H2_PAL_ERR_FORMAT;
    }
    const char *cursor = skip_space(response->lines[0] + 6u);
    if (*cursor++ != '(') { return H2_PAL_ERR_FORMAT; }
    int found = 0;
    for (;;) {
        char storage[16];
        if (!parse_storage(&cursor, storage, sizeof(storage))) { return H2_PAL_ERR_FORMAT; }
        if (strcmp(storage, "EN") == 0) { found = 1; }
        if (*cursor == ')') {
            cursor = skip_space(cursor + 1u);
            if (*cursor != '\0') { return H2_PAL_ERR_FORMAT; }
            return found ? H2_PAL_OK : H2_PAL_ERR_UNSUPPORTED;
        }
        if (!take_comma(&cursor)) { return H2_PAL_ERR_FORMAT; }
    }
}

static h2_pal_result_t parse_phonebook_status(const h2_quectel_response_t *response,
    char *storage, size_t storage_capacity, uint32_t *used, uint32_t *total) {
    if (response->count != 1u || strncmp(response->lines[0], "+CPBS:", 6u) != 0) {
        return H2_PAL_ERR_FORMAT;
    }
    const char *cursor = response->lines[0] + 6u;
    if (!parse_storage(&cursor, storage, storage_capacity) || !take_comma(&cursor) ||
        !parse_uint(&cursor, used) || !take_comma(&cursor) || !parse_uint(&cursor, total) ||
        *cursor != '\0' || *used > *total) { return H2_PAL_ERR_FORMAT; }
    return H2_PAL_OK;
}

typedef struct phonebook_range { uint32_t first, last; } phonebook_range_t;

static h2_pal_result_t parse_ranges(const h2_quectel_response_t *response,
    phonebook_range_t *ranges, size_t *out_count) {
    if (response->count != 1u || strncmp(response->lines[0], "+CPBR:", 6u) != 0) {
        return H2_PAL_ERR_FORMAT;
    }
    const char *cursor = skip_space(response->lines[0] + 6u);
    if (*cursor++ != '(') { return H2_PAL_ERR_FORMAT; }
    size_t count = 0u;
    for (;;) {
        if (count == PHONEBOOK_RANGES_MAX) { return H2_PAL_ERR_TRUNCATED; }
        phonebook_range_t *range = &ranges[count];
        if (!parse_uint(&cursor, &range->first)) { return H2_PAL_ERR_FORMAT; }
        range->last = range->first;
        if (*cursor == '-') {
            cursor++;
            if (!parse_uint(&cursor, &range->last)) { return H2_PAL_ERR_FORMAT; }
        }
        if (range->last < range->first || range->last > INT_MAX) {
            return H2_PAL_ERR_FORMAT;
        }
        count++;
        if (*cursor == ')') { cursor = skip_space(cursor + 1u); break; }
        if (!take_comma(&cursor)) { return H2_PAL_ERR_FORMAT; }
    }
    uint32_t number_length, text_length;
    if (!take_comma(&cursor) || !parse_uint(&cursor, &number_length) ||
        !take_comma(&cursor) || !parse_uint(&cursor, &text_length) ||
        *cursor != '\0' || number_length == 0u) { return H2_PAL_ERR_FORMAT; }
    /* A compound supported-index list need not be ordered. Normalize it so
     * each supported slot is read once, including overlapping intervals. */
    for (size_t i = 1u; i < count; i++) {
        phonebook_range_t value = ranges[i];
        size_t j = i;
        while (j != 0u && ranges[j - 1u].first > value.first) {
            ranges[j] = ranges[j - 1u];
            j--;
        }
        ranges[j] = value;
    }
    size_t merged = 0u;
    for (size_t i = 0u; i < count; i++) {
        if (merged != 0u && ranges[i].first <= ranges[merged - 1u].last + 1u) {
            if (ranges[i].last > ranges[merged - 1u].last) { ranges[merged - 1u].last = ranges[i].last; }
        } else { ranges[merged++] = ranges[i]; }
    }
    uint64_t slots = 0u;
    for (size_t i = 0u; i < merged; i++) { slots += (uint64_t)ranges[i].last - ranges[i].first + 1u; }
    if (slots > PHONEBOOK_SCAN_MAX) { return H2_PAL_ERR_TRUNCATED; }
    *out_count = merged;
    return H2_PAL_OK;
}

typedef struct phonebook_query {
    h2_pal_modem_emergency_number_t *numbers;
    size_t capacity, count;
    uint32_t first, last, last_index;
    uint8_t index_seen;
    h2_pal_result_t result;
} phonebook_query_t;

static h2_pal_result_t parse_entry(phonebook_query_t *query, const char *line) {
    if (strncmp(line, "+CPBR:", 6u) != 0) { return H2_PAL_ERR_FORMAT; }
    const char *cursor = line + 6u;
    uint32_t index, type;
    if (!parse_uint(&cursor, &index) || !take_comma(&cursor) ||
        index < query->first || index > query->last ||
        (query->index_seen && index <= query->last_index) || *cursor++ != '"') {
        return H2_PAL_ERR_FORMAT;
    }
    const char *number = cursor;
    while (h2_quectel_ascii_digit((unsigned char)*cursor)) { cursor++; }
    size_t length = (size_t)(cursor - number);
    if (length == 0u || *cursor != '"') { return H2_PAL_ERR_FORMAT; }
    if (length >= H2_PAL_MODEM_PHONE_NUMBER_MAX) { return H2_PAL_ERR_TRUNCATED; }
    cursor = skip_space(cursor + 1u);
    if (!take_comma(&cursor) || !parse_uint(&cursor, &type) || type > 255u ||
        !take_comma(&cursor) || *cursor != '"') { return H2_PAL_ERR_FORMAT; }
    const char *end = strrchr(cursor + 1u, '"');
    if (end == NULL || *skip_space(end + 1u) != '\0') { return H2_PAL_ERR_FORMAT; }
    if (query->count == query->capacity) { return H2_PAL_ERR_TRUNCATED; }
    h2_pal_modem_emergency_number_t *entry = &query->numbers[query->count++];
    memset(entry, 0, sizeof(*entry));
    memcpy(entry->number, number, length);
    /* EN combines (U)SIM or ME numbers. Neither physical origin nor SIM
     * applicability can be inferred from the storage name or current SIM. */
    entry->scope = H2_PAL_MODEM_EMERGENCY_SCOPE_UNKNOWN;
    entry->source = H2_PAL_MODEM_EMERGENCY_SOURCE_UNKNOWN;
    query->index_seen = 1u;
    query->last_index = index;
    return H2_PAL_OK;
}

static void collect_entry(void *user, const char *line) {
    phonebook_query_t *query = user;
    if (query->result == H2_PAL_OK) { query->result = parse_entry(query, line); }
}

h2_pal_result_t h2_quectel_ec800m_restore_phonebook(h2_quectel_modem_t *modem,
    uint32_t timeout_ms) {
    if (!modem->phonebook_restore_pending) { return H2_PAL_OK; }
    if (modem->family != H2_QUECTEL_MODEM_FAMILY_EC800M) { return H2_PAL_ERR_UNSUPPORTED; }
    char command[48];
    (void)snprintf(command, sizeof(command), "AT+CPBS=\"%s\"", modem->phonebook_restore_storage);
    h2_quectel_response_t response;
    h2_pal_result_t rc = exchange(modem, command, timeout_ms, &response, 0, NULL, NULL);
    if (rc == H2_PAL_OK) {
        modem->phonebook_restore_pending = 0u;
        memset(modem->phonebook_restore_storage, 0, sizeof(modem->phonebook_restore_storage));
    }
    return rc;
}

h2_pal_result_t h2_quectel_ec800m_emergency_numbers(h2_quectel_modem_t *modem,
    uint32_t timeout_ms, h2_pal_modem_emergency_number_t *out_numbers,
    size_t capacity, size_t *out_count) {
    h2_pal_result_t rc = h2_quectel_ec800m_restore_phonebook(modem, timeout_ms);
    if (rc != H2_PAL_OK) { return rc; }
    h2_quectel_response_t response;
    rc = exchange(modem, "AT+CPBS=?", timeout_ms, &response, 1, NULL, NULL);
    if (rc == H2_PAL_OK) { rc = supports_emergency_phonebook(&response); }
    if (rc != H2_PAL_OK) { return rc; }
    rc = exchange(modem, "AT+CPBS?", timeout_ms, &response, 0, NULL, NULL);
    char storage[16];
    uint32_t used = 0u, total = 0u;
    if (rc == H2_PAL_OK) { rc = parse_phonebook_status(&response, storage, sizeof(storage), &used, &total); }
    if (rc != H2_PAL_OK) { return rc; }
    const uint32_t reset_generation = modem->reset_generation;
    const uint32_t sim_generation = modem->sim_generation;
    if (strcmp(storage, "EN") != 0) {
        memcpy(modem->phonebook_restore_storage, storage, strlen(storage) + 1u);
        /* A failed selection might still have been applied by the module. */
        modem->phonebook_restore_pending = 1u;
        rc = exchange(modem, "AT+CPBS=\"EN\"", timeout_ms, &response, 0, NULL, NULL);
        if (rc == H2_PAL_OK) { rc = exchange(modem, "AT+CPBS?", timeout_ms, &response, 0, NULL, NULL); }
        if (rc == H2_PAL_OK) { rc = parse_phonebook_status(&response, storage, sizeof(storage), &used, &total); }
        if (rc == H2_PAL_OK && strcmp(storage, "EN") != 0) { rc = H2_PAL_ERR_INVALID_STATE; }
    }
    phonebook_query_t query = {.numbers = out_numbers, .capacity = capacity};
    if (rc == H2_PAL_OK && used > capacity) { rc = H2_PAL_ERR_TRUNCATED; }
    if (rc == H2_PAL_OK && used != 0u) {
        phonebook_range_t ranges[PHONEBOOK_RANGES_MAX];
        size_t range_count = 0u;
        rc = exchange(modem, "AT+CPBR=?", timeout_ms, &response, 0, NULL, NULL);
        if (rc == H2_PAL_OK) { rc = parse_ranges(&response, ranges, &range_count); }
        for (size_t i = 0u; rc == H2_PAL_OK && i < range_count; i++) {
            for (uint32_t first = ranges[i].first; rc == H2_PAL_OK;) {
                uint32_t last = ranges[i].last - first >= PHONEBOOK_PAGE_SIZE
                    ? first + PHONEBOOK_PAGE_SIZE - 1u : ranges[i].last;
                char command[64];
                (void)snprintf(command, sizeof(command), "AT+CPBR=%lu,%lu",
                    (unsigned long)first, (unsigned long)last);
                query.first = first;
                query.last = last;
                rc = exchange(modem, command, timeout_ms, &response, 0, collect_entry, &query);
                if (rc == H2_PAL_OK) { rc = query.result; }
                if (last == ranges[i].last) { break; }
                first = last + 1u;
            }
        }
    }
    if (rc == H2_PAL_OK && query.count != used) { rc = H2_PAL_ERR_TRUNCATED; }
    if (reset_generation != modem->reset_generation || sim_generation != modem->sim_generation) {
        /* Do not replay a saved selection against an invalidated session.
         * Recovery will first re-identify the model, then retry restoration. */
        rc = H2_PAL_ERR_INVALID_STATE;
    } else {
        h2_pal_result_t restore = h2_quectel_ec800m_restore_phonebook(modem, timeout_ms);
        if (restore != H2_PAL_OK) { rc = restore; }
    }
    if (rc == H2_PAL_OK) { *out_count = query.count; }
    return rc;
}

int h2_quectel_ec800m_parse_rsrp(const char *line, int32_t *out_rsrp) {
    if (line == NULL || strncmp(line, "+QENG:", 6u) != 0) { return 0; }
    const char *fields[18];
    size_t lengths[18];
    const char *cursor = skip_space(line + 6u);
    for (size_t i = 0u; i < 18u; i++) {
        fields[i] = cursor;
        while (*cursor != '\0' && *cursor != ',') { cursor++; }
        const char *end = cursor;
        while (end > fields[i] && h2_quectel_ascii_space((unsigned char)end[-1])) { end--; }
        lengths[i] = (size_t)(end - fields[i]);
        if (i == 17u) { if (*cursor != '\0') { return 0; } }
        else { if (*cursor != ',') { return 0; } cursor = skip_space(cursor + 1u); }
    }
    if (lengths[0] != 13u || strncmp(fields[0], "\"servingcell\"", 13u) != 0 ||
        lengths[2] != 5u || strncmp(fields[2], "\"LTE\"", 5u) != 0 ||
        !((lengths[1] == 8u && strncmp(fields[1], "\"NOCONN\"", 8u) == 0) ||
          (lengths[1] == 9u && strncmp(fields[1], "\"CONNECT\"", 9u) == 0) ||
          (lengths[1] == 8u && strncmp(fields[1], "\"LIMSRV\"", 8u) == 0))) {
        return 0;
    }
    if (lengths[13] == 0u || lengths[13] >= 16u) { return 0; }
    char value[16];
    memcpy(value, fields[13], lengths[13]);
    value[lengths[13]] = '\0';
    char *end = NULL;
    long rsrp = strtol(value, &end, 10);
    if (end == value || *end != '\0' || rsrp < -156 || rsrp > -31) { return 0; }
    *out_rsrp = (int32_t)rsrp;
    return 1;
}
