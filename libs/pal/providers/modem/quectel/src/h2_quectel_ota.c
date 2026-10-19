#include "h2_quectel_internal.h"

#include <stdio.h>
#include <string.h>

static int terminal_error(const h2_quectel_response_t *response) {
    return h2_quectel_response_find(response, "ERROR") != NULL ||
        h2_quectel_response_find(response, "+CME ERROR:") != NULL ||
        h2_quectel_response_find(response, "+CMS ERROR:") != NULL;
}

static int probe_unsupported(const h2_quectel_response_t *response) {
    const char *cme = h2_quectel_response_find(response, "+CME ERROR:");
    if (h2_quectel_response_find(response, "ERROR") != NULL) { return 1; }
    if (cme == NULL) { return 0; }
    cme += 11;
    while (*cme == ' ') { cme++; }
    return strcmp(cme, "4") == 0 || strcmp(cme, "operation not supported") == 0;
}

h2_pal_result_t h2_quectel_read_revision(h2_quectel_modem_t *modem,
    uint32_t timeout_ms, char *out_revision, size_t capacity) {
    if (out_revision == NULL || capacity == 0u) { return H2_PAL_ERR_INVALID_ARG; }
    out_revision[0] = '\0';
    h2_quectel_response_t response = {0};
    const uint32_t reset_generation = modem->reset_generation;
    const uint32_t sim_generation = modem->sim_generation;
    h2_pal_result_t rc = h2_quectel_at_exchange_timeout(modem, "AT+CGMR", &response, 0, timeout_ms);
    if (rc != H2_PAL_OK) { return rc; }
    if (reset_generation != modem->reset_generation || sim_generation != modem->sim_generation) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (terminal_error(&response)) { return H2_PAL_ERR_IO; }
    if (response.truncated) { return H2_PAL_ERR_TRUNCATED; }
    if (!response.ok || response.count != 1u ||
        !h2_pal_modem_ota_revision_valid(response.lines[0])) { return H2_PAL_ERR_FORMAT; }
    const size_t length = strlen(response.lines[0]);
    if (length >= capacity) { return H2_PAL_ERR_TRUNCATED; }
    memcpy(out_revision, response.lines[0], length + 1u);
    return H2_PAL_OK;
}

static h2_pal_result_t validate_url(const char *url, size_t *out_length) {
    if (url == NULL) { return H2_PAL_ERR_INVALID_ARG; }
    size_t length = 0u;
    while (length <= 255u && url[length] != '\0') {
        const unsigned char ch = (unsigned char)url[length];
        if (ch <= 0x20u || ch > 0x7eu || ch == '"' || ch == '\\' || ch == '#') {
            return H2_PAL_ERR_INVALID_ARG;
        }
        length++;
    }
    if (length == 0u || length > 255u) { return H2_PAL_ERR_INVALID_ARG; }
    const char *host = NULL;
    if (strncmp(url, "http://", 7u) == 0) { host = url + 7; }
    else if (strncmp(url, "https://", 8u) == 0) { host = url + 8; }
    else if (strncmp(url, "ftp://", 6u) == 0 || strncmp(url, "FTP://", 6u) == 0) { host = url + 6; }
    if (host == NULL || *host == '\0' || *host == '/' || *host == '?' || strchr(host, '/') == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_length = length;
    return H2_PAL_OK;
}

static int path_ends_with(const char *url, const char *suffix) {
    const size_t path_length = strcspn(url, "?");
    const size_t suffix_length = strlen(suffix);
    return path_length >= suffix_length &&
        memcmp(url + path_length - suffix_length, suffix, suffix_length) == 0;
}

/* CGMM alone cannot distinguish DFOTA from MiniFOTA. Firmware/package names
 * encode flash size as M02/M04/M08/M16. Unknown names use the smaller URL bound. */
static unsigned flash_size(const char *revision) {
    for (const char *p = revision; *p != '\0'; p++) {
        if (*p != 'M' || strlen(p) < 3u || (p[3] != '\0' && p[3] != '_')) { continue; }
        if (memcmp(p, "M02", 3u) == 0) { return 2u; }
        if (memcmp(p, "M04", 3u) == 0) { return 4u; }
        if (memcmp(p, "M08", 3u) == 0) { return 8u; }
        if (memcmp(p, "M16", 3u) == 0) { return 16u; }
    }
    return 0u;
}

static void new_request(h2_quectel_modem_t *modem, const char *source, const char *target) {
    uint32_t attempt_id = modem->ota_status.attempt_id + 1u;
    if (attempt_id == 0u) { attempt_id = 1u; }
    memset(&modem->ota_status, 0, sizeof(modem->ota_status));
    modem->ota_status.attempt_id = attempt_id;
    memcpy(modem->ota_status.source_revision, source, strlen(source) + 1u);
    memcpy(modem->ota_status.target_revision, target, strlen(target) + 1u);
    modem->ota_activity_seen = 0u;
    modem->ota_end_seen = 0u;
    modem->ota_ready_seen = 0u;
    modem->ota_mini = 0u;
    modem->ota_flash_started = 0u;
}

h2_pal_result_t h2_quectel_ota_start(void *user, const h2_pal_modem_ota_request_t *request) {
    if (request == NULL || !h2_pal_modem_ota_revision_valid(request->target_revision) ||
        (request->expected_revision != NULL &&
         !h2_pal_modem_ota_revision_valid(request->expected_revision))) { return H2_PAL_ERR_INVALID_ARG; }
    size_t url_length;
    h2_pal_result_t rc = validate_url(request->url, &url_length);
    if (rc != H2_PAL_OK) { return rc; }
    h2_quectel_modem_t *modem = user;
    rc = h2_quectel_operation_begin(modem);
    if (rc != H2_PAL_OK) { return rc; }
    if (!modem->opened) { return h2_quectel_operation_end(modem, H2_PAL_ERR_CLOSED); }
    if (modem->ota_hold || modem->call_hold || modem->gnss_hold || modem->data_hold ||
        modem->data_status.state != H2_PAL_MODEM_DATA_CLOSED || modem->phonebook_restore_pending) {
        return h2_quectel_operation_end(modem, H2_PAL_ERR_BUSY);
    }
    rc = h2_quectel_modem_prepare(modem);
    if (rc == H2_PAL_OK) { rc = h2_quectel_resolve_model(modem); }
    if (rc != H2_PAL_OK) { return h2_quectel_operation_end(modem, rc); }
    if ((modem->capabilities & H2_PAL_MODEM_CAPABILITY_OTA) == 0u) {
        return h2_quectel_operation_end(modem, H2_PAL_ERR_UNSUPPORTED);
    }
    char revision[H2_PAL_MODEM_IDENTITY_MAX];
    rc = h2_quectel_read_revision(modem, request->timeout_ms, revision, sizeof(revision));
    if (rc != H2_PAL_OK) { return h2_quectel_operation_end(modem, rc); }
    if (strcmp(revision, request->target_revision) == 0) {
        new_request(modem, revision, request->target_revision);
        memcpy(modem->ota_status.observed_revision, revision, strlen(revision) + 1u);
        modem->ota_status.state = H2_PAL_MODEM_OTA_SUCCEEDED;
        return h2_quectel_operation_end(modem, H2_PAL_OK);
    }
    if (request->expected_revision != NULL && strcmp(revision, request->expected_revision) != 0) {
        return h2_quectel_operation_end(modem, H2_PAL_ERR_INVALID_STATE);
    }
    const int ec800m = modem->family == H2_QUECTEL_MODEM_FAMILY_EC800M;
    const unsigned flash = ec800m ? flash_size(revision) : 0u;
    const int mini = ec800m && (flash == 2u || flash == 4u || flash == 8u ||
        path_ends_with(request->url, ".mini_1"));
    if (path_ends_with(request->url, ".mini_2") ||
        (mini && !path_ends_with(request->url, ".mini_1")) ||
        (!ec800m && path_ends_with(request->url, ".mini_1")) ||
        (ec800m && flash == 16u && mini) ||
        (ec800m && flash != 16u && url_length > 128u)) {
        return h2_quectel_operation_end(modem, H2_PAL_ERR_INVALID_ARG);
    }
    if (ec800m && ((flash == 2u && strncmp(request->url, "https://", 8u) == 0) ||
        ((flash == 2u || flash == 4u) &&
         (strncmp(request->url, "ftp://", 6u) == 0 || strncmp(request->url, "FTP://", 6u) == 0)))) {
        return h2_quectel_operation_end(modem, H2_PAL_ERR_UNSUPPORTED);
    }
    h2_quectel_response_t response = {0};
    const uint32_t reset_generation = modem->reset_generation;
    const uint32_t sim_generation = modem->sim_generation;
    rc = h2_quectel_at_exchange_timeout(modem, "AT+QFOTADL=?", &response, 0, request->timeout_ms);
    if (reset_generation != modem->reset_generation || sim_generation != modem->sim_generation) {
        return h2_quectel_operation_end(modem, H2_PAL_ERR_INVALID_STATE);
    }
    if (response.count != 0u && probe_unsupported(&response)) {
        modem->capabilities &= ~(uint32_t)H2_PAL_MODEM_CAPABILITY_OTA;
        return h2_quectel_operation_end(modem, H2_PAL_ERR_UNSUPPORTED);
    }
    if (response.count != 0u && terminal_error(&response)) { return h2_quectel_operation_end(modem, H2_PAL_ERR_IO); }
    if (rc != H2_PAL_OK || response.truncated || !response.ok) {
        return h2_quectel_operation_end(modem, rc != H2_PAL_OK ? rc : H2_PAL_ERR_FORMAT);
    }
    if (modem->call_hold || modem->gnss_hold || modem->data_hold ||
        modem->data_status.state != H2_PAL_MODEM_DATA_CLOSED) {
        return h2_quectel_operation_end(modem, H2_PAL_ERR_BUSY);
    }
    /* EC25 has no optional progress arguments. MiniFOTA rejects these args;
     * normal EC800M DFOTA explicitly requests 100 download reports so the
     * reported counter equals percent. Never pass a second MiniFOTA URL. */
    char command[H2_QUECTEL_COMMAND_MAX];
    const int n = snprintf(command, sizeof(command), "AT+QFOTADL=\"%s\"%s", request->url,
        ec800m && !mini ? ",0,100" : "");
    if (n <= 0 || (size_t)n >= sizeof(command)) {
        return h2_quectel_operation_end(modem, H2_PAL_ERR_INVALID_ARG);
    }
    new_request(modem, revision, request->target_revision);
    modem->ota_mini = (uint8_t)mini;
    modem->ota_status.state = H2_PAL_MODEM_OTA_STARTING;
    modem->ota_hold = 1u;
    modem->ota_command_allowed = 1u;
    rc = h2_quectel_at_exchange_timeout(modem, command, &response, 0, request->timeout_ms);
    memset(command, 0, sizeof(command)); /* URL may carry credentials. */
    modem->ota_command_allowed = 0u;
    if (rc == H2_PAL_OK && (terminal_error(&response) || !response.ok || response.truncated)) {
        rc = terminal_error(&response) ? H2_PAL_ERR_IO : H2_PAL_ERR_FORMAT;
    }
    if (rc != H2_PAL_OK) {
        modem->ota_status.last_error = rc;
        if (terminal_error(&response) && !modem->ota_activity_seen) {
            modem->ota_status.state = H2_PAL_MODEM_OTA_FAILED;
            modem->ota_hold = 0u;
        } else if (modem->ota_status.state == H2_PAL_MODEM_OTA_STARTING) {
            /* It may already have rebooted/accepted the request. Do not send
             * it again or permit teardown after an uncertain transport result. */
            modem->ota_status.state = H2_PAL_MODEM_OTA_UNKNOWN;
        }
    }
    return h2_quectel_operation_end(modem, rc);
}

h2_pal_result_t h2_quectel_ota_get_status(void *user, uint32_t timeout_ms,
    h2_pal_modem_ota_status_t *out_status) {
    if (out_status == NULL) { return H2_PAL_ERR_INVALID_ARG; }
    memset(out_status, 0, sizeof(*out_status));
    h2_quectel_modem_t *modem = user;
    h2_pal_result_t rc = h2_quectel_operation_begin(modem);
    if (rc != H2_PAL_OK) { return rc; }
    if (modem->ota_hold && modem->ota_end_seen && modem->ota_ready_seen) {
        char revision[H2_PAL_MODEM_IDENTITY_MAX];
        modem->ota_command_allowed = 1u;
        rc = h2_quectel_read_revision(modem, timeout_ms, revision, sizeof(revision));
        modem->ota_command_allowed = 0u;
        if (rc == H2_PAL_OK) {
            memcpy(modem->ota_status.observed_revision, revision, strlen(revision) + 1u);
            const int matched = modem->ota_status.vendor_code == 0 &&
                strcmp(revision, modem->ota_status.target_revision) == 0;
            modem->ota_status.state = matched ? H2_PAL_MODEM_OTA_SUCCEEDED : H2_PAL_MODEM_OTA_FAILED;
            modem->ota_status.last_error = matched ? H2_PAL_OK : H2_PAL_ERR_IO;
            /* A MiniFOTA error can leave the module in its download system.
             * A version response alone does not prove that both stages ended. */
            if (!modem->ota_mini || modem->ota_status.vendor_code == 0) { modem->ota_hold = 0u; }
        } else {
            modem->ota_status.last_error = rc;
        }
    }
    *out_status = modem->ota_status;
    rc = h2_quectel_operation_end(modem, H2_PAL_OK);
    if (rc != H2_PAL_OK) { memset(out_status, 0, sizeof(*out_status)); }
    return rc;
}

int h2_quectel_is_ota_urc(const char *line) {
    if (line == NULL || strncmp(line, "+QIND:", 6u) != 0) { return 0; }
    const char *p = line + 6;
    while (*p == ' ' || *p == '\t') { p++; }
    if (strncmp(p, "\"FOTA\"", 6u) != 0) { return 0; }
    p += 6;
    while (*p == ' ' || *p == '\t') { p++; }
    return *p == ',';
}

static int number_argument(const char *p, uint32_t max, uint32_t *out) {
    while (*p == ' ' || *p == '\t') { p++; }
    if (*p++ != ',') { return 0; }
    while (*p == ' ' || *p == '\t') { p++; }
    if (!h2_quectel_ascii_digit((unsigned char)*p)) { return 0; }
    uint32_t value = 0u;
    do {
        const uint32_t digit = (uint32_t)(*p++ - '0');
        if (value > max / 10u || (value == max / 10u && digit > max % 10u)) { return 0; }
        value = value * 10u + digit;
    } while (h2_quectel_ascii_digit((unsigned char)*p));
    while (*p == ' ' || *p == '\t') { p++; }
    if (*p != '\0') { return 0; }
    *out = value;
    return 1;
}

void h2_quectel_ota_urc(h2_quectel_modem_t *modem, const char *line) {
    if (!modem->ota_hold) { return; }
    char event[24];
    int consumed = 0;
    if (sscanf(line, "+QIND: \"FOTA\" , \"%23[^\"]\" %n", event, &consumed) != 1 || consumed == 0) { return; }
    const char *argument = line + consumed;
    h2_pal_modem_ota_status_t *status = &modem->ota_status;
    if (strcmp(event, "HTTPSTART") == 0 || strcmp(event, "FTPSTART") == 0 || strcmp(event, "START") == 0) {
        if (*argument != '\0') { return; }
        modem->ota_activity_seen = 1u;
        if (strcmp(event, "START") == 0) { modem->ota_flash_started = 1u; }
        modem->ota_end_seen = 0u;
        modem->ota_ready_seen = 0u;
        status->last_error = H2_PAL_OK;
        status->vendor_code = 0;
        const h2_pal_modem_ota_state_t next = strcmp(event, "START") == 0
            ? H2_PAL_MODEM_OTA_UPDATING : H2_PAL_MODEM_OTA_DOWNLOADING;
        if (status->state != next) { status->progress_percent = 0u; status->progress_valid = 0u; }
        status->state = next;
        return;
    }
    const int progress = strcmp(event, "DOWNLOADING") == 0 || strcmp(event, "UPDATING") == 0;
    uint32_t value;
    if (!number_argument(argument, progress ? 100u : INT32_MAX, &value)) { return; }
    if (progress) {
        if (modem->ota_end_seen) { return; }
        const h2_pal_modem_ota_state_t next = strcmp(event, "UPDATING") == 0
            ? H2_PAL_MODEM_OTA_UPDATING : H2_PAL_MODEM_OTA_DOWNLOADING;
        modem->ota_activity_seen = 1u;
        if (next == H2_PAL_MODEM_OTA_UPDATING) { modem->ota_flash_started = 1u; }
        if (status->state != next || !status->progress_valid || value > status->progress_percent) {
            status->progress_percent = value;
        }
        status->state = next;
        status->progress_valid = 1u;
    } else if (strcmp(event, "HTTPEND") == 0 || strcmp(event, "FTPEND") == 0) {
        if (!modem->ota_activity_seen || modem->ota_end_seen) { return; }
        status->vendor_code = (int32_t)value;
        status->progress_valid = 0u;
        if (value != 0u) {
            const int uncertain = modem->ota_mini || modem->ota_flash_started;
            status->state = uncertain ? H2_PAL_MODEM_OTA_UNKNOWN : H2_PAL_MODEM_OTA_FAILED;
            status->last_error = H2_PAL_ERR_IO;
            if (!uncertain) { modem->ota_hold = 0u; }
        } else {
            status->state = H2_PAL_MODEM_OTA_UPDATING;
        }
    } else if (strcmp(event, "END") == 0 && modem->ota_activity_seen) {
        if (modem->ota_end_seen && status->vendor_code == (int32_t)value) { return; }
        modem->ota_end_seen = 1u;
        modem->ota_ready_seen = 0u;
        status->state = H2_PAL_MODEM_OTA_VERIFYING;
        status->progress_valid = 0u;
        status->vendor_code = (int32_t)value;
        status->last_error = value == 0u ? H2_PAL_OK : H2_PAL_ERR_IO;
    }
}
