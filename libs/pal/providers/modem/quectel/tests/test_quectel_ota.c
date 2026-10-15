#include "h2_quectel_modem.h"
#include "h2/pal/h2_pal_unsupported.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Private exchange symbol: verify its early-failure output contract. */
extern h2_pal_result_t h2_quectel_at_exchange_timeout(h2_quectel_modem_t *, const char *,
    h2_quectel_response_t *, int, uint32_t);
static int fail_operation_lock;
struct h2_pal_mutex { unsigned depth; int operation; };
static h2_pal_result_t mutex_create(void *u, const h2_pal_mutex_config_t *c, h2_pal_mutex_t **out) {
    (void)u;
    *out = calloc(1u, sizeof(**out));
    if (*out != NULL) { (*out)->operation = strcmp(c->name, "quectel/operation") == 0; }
    return *out != NULL ? H2_PAL_OK : H2_PAL_ERR_NO_MEMORY;
}
static h2_pal_result_t mutex_destroy(void *u, h2_pal_mutex_t *m) {
    (void)u; assert(m->depth == 0u); free(m); return H2_PAL_OK;
}
static h2_pal_result_t mutex_lock(void *u, h2_pal_mutex_t *m) {
    (void)u;
    if (m->operation && fail_operation_lock) { fail_operation_lock = 0; return H2_PAL_ERR_IO; }
    m->depth++; return H2_PAL_OK;
}
static h2_pal_result_t mutex_unlock(void *u, h2_pal_mutex_t *m) {
    (void)u; assert(m->depth != 0u); m->depth--; return H2_PAL_OK;
}
static const h2_pal_sync_vtable_t sync_vtable = {
    .create_mutex = mutex_create, .destroy_mutex = mutex_destroy,
    .lock_mutex = mutex_lock, .unlock_mutex = mutex_unlock,
};
static const h2_pal_sync_api_t sync_api = {.vtable = &sync_vtable};

typedef struct fixture {
    h2_quectel_modem_t modem;
    const char *model, *revision, *revision_reply, *probe_reply, *start_reply;
    const char *interrupt;
    h2_pal_result_t revision_result, probe_result, start_result;
    unsigned revisions, probes, starts, deinits, count;
    uint32_t start_timeout;
    int asleep;
    int raw_echo;
    int fail_after_revision, fail_after_probe;
    unsigned wakes, fail_wake_at;
    char last_start[H2_QUECTEL_COMMAND_MAX];
    char raw_reply[1024];
    size_t raw_offset;
} fixture_t;

static h2_pal_result_t exchange(fixture_t *f, const char *cmd, char *out, size_t capacity, uint32_t timeout) {
    f->count++;
    h2_pal_result_t rc = H2_PAL_OK;
    const char *reply = "OK\r\n";
    if (strcmp(cmd, "AT+CGMM") == 0) {
        snprintf(out, capacity, "%s\r\nOK\r\n", f->model);
        return H2_PAL_OK;
    }
    if (strcmp(cmd, "AT+CGMR") == 0) {
        f->revisions++;
        if (f->fail_after_revision) { f->fail_after_revision = 0; fail_operation_lock = 1; }
        if (f->revision_reply == NULL) {
            snprintf(out, capacity, "%s\r\nOK\r\n", f->revision);
            return f->revision_result;
        }
        reply = f->revision_reply;
        rc = f->revision_result;
    } else if (strcmp(cmd, "AT+QFOTADL=?") == 0) {
        f->probes++;
        if (f->fail_after_probe) { f->fail_after_probe = 0; fail_operation_lock = 1; }
        reply = f->probe_reply;
        rc = f->probe_result;
    } else if (strncmp(cmd, "AT+QFOTADL=", 11u) == 0) {
        f->starts++;
        assert(strlen(cmd) < sizeof(f->last_start));
        strcpy(f->last_start, cmd);
        f->start_timeout = timeout;
        reply = f->start_reply;
        rc = f->start_result;
        if (f->interrupt != NULL) {
            h2_quectel_handle_urc_line(&f->modem, "+QIND: \"FOTA\",\"START\"");
            h2_quectel_handle_urc_line(&f->modem, f->interrupt);
        }
    }
    snprintf(out, capacity, "%s", reply);
    return rc;
}
static h2_pal_result_t command(void *u, const char *cmd, char *out, size_t cap, uint32_t timeout) {
    fixture_t *f = u;
    assert(f->modem.lock->depth == 0u && f->modem.operation_lock->depth != 0u && !f->asleep);
    return exchange(f, cmd, out, cap, timeout);
}
static h2_pal_result_t raw_write(void *u, const uint8_t *bytes, size_t length, uint32_t timeout, size_t *written) {
    fixture_t *f = u;
    assert(length < H2_QUECTEL_COMMAND_MAX && bytes[length - 1u] == '\r');
    char cmd[H2_QUECTEL_COMMAND_MAX];
    memcpy(cmd, bytes, length - 1u);
    cmd[length - 1u] = '\0';
    f->raw_offset = 0u;
    *written = length;
    h2_pal_result_t rc = exchange(f, cmd, f->raw_reply, sizeof(f->raw_reply), timeout);
    if (f->raw_echo) {
        const size_t reply_length = strlen(f->raw_reply);
        assert(length + 1u + reply_length < sizeof(f->raw_reply));
        memmove(f->raw_reply + length + 1u, f->raw_reply, reply_length + 1u);
        memcpy(f->raw_reply, bytes, length);
        f->raw_reply[length] = '\n';
    }
    return rc;
}
static h2_pal_result_t raw_read(void *u, uint8_t *out, size_t length, uint32_t timeout, size_t *received) {
    fixture_t *f = u;
    (void)timeout; assert(length == 1u);
    if (f->raw_reply[f->raw_offset] == '\0') { *received = 0u; return H2_PAL_ERR_TIMEOUT; }
    *out = (uint8_t)f->raw_reply[f->raw_offset++];
    *received = 1u;
    return H2_PAL_OK;
}
static h2_pal_result_t gate(void *u, int asleep) {
    fixture_t *f = u;
    if (!asleep && ++f->wakes == f->fail_wake_at) { return H2_PAL_ERR_IO; }
    f->asleep = asleep; return H2_PAL_OK;
}
static h2_pal_result_t deinit(void *u) { ((fixture_t *)u)->deinits++; return H2_PAL_OK; }
static void init(fixture_t *f, const char *model, const char *revision, int raw) {
    memset(f, 0, sizeof(*f));
    f->model = model; f->revision = revision;
    f->probe_reply = f->start_reply = "OK\r\n";
    const h2_quectel_modem_config_t config = {
        .transport_user = f, .command = raw ? NULL : command,
        .read = raw ? raw_read : NULL, .write = raw ? raw_write : NULL,
        .deinit = deinit, .sync_api = &sync_api,
        .profile = H2_QUECTEL_MODEM_PROFILE_EC25_UART,
        .sleep_gate = raw ? NULL : gate,
    };
    assert(h2_quectel_modem_init(&f->modem, &config) == H2_PAL_OK);
    assert(h2_pal_modem_open(&f->modem.platform, 0u) == H2_PAL_OK);
    f->count = 0u;
}
static h2_pal_modem_ota_status_t status(fixture_t *f) {
    h2_pal_modem_ota_status_t out;
    assert(h2_pal_modem_ota_get_status(&f->modem.platform, 321u, &out) == H2_PAL_OK);
    assert(f->modem.operation_depth == 0u && f->modem.lock->depth == 0u && f->modem.operation_lock->depth == 0u);
    return out;
}
static void finish(fixture_t *f) {
    if (f->modem.ota_hold) {
        f->interrupt = NULL;
        f->revision_reply = NULL; f->revision_result = H2_PAL_OK;
        f->revision = f->modem.ota_status.target_revision;
        h2_quectel_handle_urc_line(&f->modem, "+QIND: \"FOTA\",\"START\"");
        h2_quectel_handle_urc_line(&f->modem, "+QIND: \"FOTA\",\"END\",0");
        h2_quectel_handle_urc_line(&f->modem, "RDY");
        assert(status(f).state == H2_PAL_MODEM_OTA_SUCCEEDED);
    }
    assert(h2_quectel_modem_deinit(&f->modem) == H2_PAL_OK);
}
static h2_pal_modem_ota_request_t request(const char *url, const char *source, const char *target) {
    const h2_pal_modem_ota_request_t r = {
        .url = url, .expected_revision = source, .target_revision = target, .timeout_ms = 777u,
    };
    return r;
}

static void test_flow(const char *model, const char *source, const char *target, const char *url, int raw) {
    fixture_t f;
    init(&f, model, source, raw);
    h2_pal_modem_identity_t identity;
    assert(h2_pal_modem_get_identity(&f.modem.platform, &identity) == H2_PAL_OK);
    assert(strcmp(identity.revision, source) == 0);
    if (!raw) {
        assert(h2_pal_modem_set_power_policy(&f.modem.platform, H2_PAL_MODEM_POWER_POLICY_AUTO_SLEEP) == H2_PAL_OK);
        assert(f.asleep);
    }
    h2_pal_modem_ota_request_t r = request(url, source, target);
    assert(h2_pal_modem_ota_start(&f.modem.platform, &r) == H2_PAL_OK);
    assert(f.starts == 1u && f.probes == 1u && f.start_timeout == 777u);
    char expected[H2_QUECTEL_COMMAND_MAX];
    snprintf(expected, sizeof(expected), "AT+QFOTADL=\"%s\"%s", url,
        strcmp(model, "EC800M") == 0 && strstr(source, "M16") != NULL ? ",0,100" : "");
    assert(strcmp(f.last_start, expected) == 0 && !f.asleep);
    assert(status(&f).state == H2_PAL_MODEM_OTA_STARTING);
    h2_quectel_handle_urc_line(&f.modem, "+QIND: \"FOTA\",\"END\",0");
    assert(status(&f).state == H2_PAL_MODEM_OTA_STARTING); /* Uncorrelated terminal result. */
    assert(h2_pal_modem_ota_start(&f.modem.platform, &r) == H2_PAL_ERR_BUSY && f.starts == 1u);
    assert(h2_pal_modem_close(&f.modem.platform, 0u) == H2_PAL_ERR_BUSY);
    assert(h2_quectel_modem_transport_closed(&f.modem) == H2_PAL_ERR_BUSY);
    assert(h2_quectel_modem_deinit(&f.modem) == H2_PAL_ERR_BUSY && f.deinits == 0u);
    const unsigned count = f.count;
    assert(h2_pal_modem_get_identity(&f.modem.platform, &identity) == H2_PAL_ERR_BUSY);
    assert(f.count == count);
    h2_quectel_handle_urc_line(&f.modem, "+QIND: \"FOTA\",\"HTTPSTART\"");
    h2_quectel_handle_urc_line(&f.modem, "+QIND: \"FOTA\",\"DOWNLOADING\",30");
    assert(status(&f).state == H2_PAL_MODEM_OTA_DOWNLOADING && status(&f).progress_percent == 30u);
    const char *malformed[] = {
        "+QIND: \"FOTA\",\"DOWNLOADING\",101", "+QIND: \"FOTA\",\"DOWNLOADING\",-1",
        "+QIND: \"FOTA\",\"DOWNLOADING\",999999999999", "+QIND: \"FOTA\",\"END\",0,trailer",
        "+QIND: \"FOTA\",\"HTTPSTART\",0", "+QIND: \"OTHER\",\"END\",0",
    };
    for (size_t i = 0u; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
        h2_quectel_handle_urc_line(&f.modem, malformed[i]);
        assert(status(&f).state == H2_PAL_MODEM_OTA_DOWNLOADING && status(&f).progress_percent == 30u);
    }
    h2_quectel_handle_urc_line(&f.modem, "+QIND: \"FOTA\",\"HTTPEND\",0");
    h2_quectel_handle_urc_line(&f.modem, "+QIND: \"FOTA\",\"START\"");
    h2_quectel_handle_urc_line(&f.modem, "+QIND: \"FOTA\",\"UPDATING\",60");
    h2_quectel_handle_urc_line(&f.modem, "RDY"); /* Intermediate MiniFOTA reboot. */
    assert(status(&f).state == H2_PAL_MODEM_OTA_UPDATING && f.modem.ota_hold);
    h2_quectel_handle_urc_line(&f.modem, "+QIND: \"FOTA\",\"UPDATING\",100");
    assert(status(&f).state == H2_PAL_MODEM_OTA_UPDATING);
    h2_quectel_handle_urc_line(&f.modem, "+QIND: \"FOTA\",\"END\",0");
    const unsigned revisions = f.revisions;
    assert(status(&f).state == H2_PAL_MODEM_OTA_VERIFYING && f.revisions == revisions);
    f.revision = target;
    h2_quectel_handle_urc_line(&f.modem, "APP RDY");
    h2_quectel_handle_urc_line(&f.modem, "+QIND: \"FOTA\",\"END\",0"); /* Duplicate must retain readiness. */
    h2_pal_modem_ota_status_t out = status(&f);
    assert(out.state == H2_PAL_MODEM_OTA_SUCCEEDED && !f.modem.ota_hold);
    assert(strcmp(out.source_revision, source) == 0 && strcmp(out.observed_revision, target) == 0);
    assert(out.attempt_id == 1u && f.revisions == revisions + 1u && out.last_error == H2_PAL_OK);
    assert(status(&f).state == H2_PAL_MODEM_OTA_SUCCEEDED && f.revisions == revisions + 1u);
    finish(&f);
}

static void test_version_guards(void) {
    fixture_t f;
    init(&f, "EC25", "EC25-R01", 0);
    h2_pal_modem_ota_request_t r = request("https://example.com/update.zip", "EC25-other", "EC25-R02");
    assert(h2_pal_modem_ota_start(&f.modem.platform, &r) == H2_PAL_ERR_INVALID_STATE);
    assert(f.starts == 0u && f.probes == 0u && status(&f).state == H2_PAL_MODEM_OTA_IDLE);
    r.target_revision = f.revision;
    assert(h2_pal_modem_ota_start(&f.modem.platform, &r) == H2_PAL_OK);
    assert(f.starts == 0u && status(&f).state == H2_PAL_MODEM_OTA_SUCCEEDED);
    r = request("https://example.com/update.zip", NULL, "EC25-R02");
    const char *bad[] = {"ERROR\r\nOK\r\n", "\r\nOK\r\n", "EC25-R01\r\nother\r\nOK\r\n", "EC25-R01\r\n"};
    for (size_t i = 0u; i < sizeof(bad) / sizeof(bad[0]); i++) {
        f.revision_reply = bad[i];
        assert(h2_pal_modem_ota_start(&f.modem.platform, &r) != H2_PAL_OK);
        h2_pal_modem_identity_t identity;
        assert(h2_pal_modem_get_identity(&f.modem.platform, &identity) == H2_PAL_OK);
        assert(identity.revision[0] == '\0' && f.starts == 0u);
    }
    f.revision_reply = NULL;
    assert(h2_pal_modem_ota_start(&f.modem.platform, &r) == H2_PAL_OK);
    h2_quectel_handle_urc_line(&f.modem, "+QIND: \"FOTA\",\"START\"");
    h2_quectel_handle_urc_line(&f.modem, "+QIND: \"FOTA\",\"END\",0");
    h2_quectel_handle_urc_line(&f.modem, "RDY");
    f.revision_result = H2_PAL_ERR_TIMEOUT;
    assert(status(&f).state == H2_PAL_MODEM_OTA_VERIFYING && status(&f).last_error == H2_PAL_ERR_TIMEOUT);
    assert(h2_pal_modem_close(&f.modem.platform, 0u) == H2_PAL_ERR_BUSY);
    f.revision_result = H2_PAL_OK;
    assert(status(&f).state == H2_PAL_MODEM_OTA_FAILED && !f.modem.ota_hold); /* Old firmware still active. */
    assert(f.starts == 1u && strcmp(status(&f).observed_revision, "EC25-R01") == 0);
    finish(&f);
}

static void test_failures(void) {
    fixture_t f;
    h2_pal_modem_ota_request_t r = request("https://example.com/update.zip", NULL, "EC25-R02");
    init(&f, "EC25", "EC25-R01", 0);
    f.probe_reply = "ERROR\r\n";
    assert(h2_pal_modem_ota_start(&f.modem.platform, &r) == H2_PAL_ERR_UNSUPPORTED);
    uint32_t capabilities;
    assert(h2_pal_modem_get_capabilities(&f.modem.platform, &capabilities) == H2_PAL_OK);
    assert(!(capabilities & H2_PAL_MODEM_CAPABILITY_OTA) && !f.starts);
    assert(h2_pal_modem_ota_start(&f.modem.platform, &r) == H2_PAL_ERR_UNSUPPORTED && f.probes == 1u);
    finish(&f);
    init(&f, "EC800M", "EC800MCNLER06A07M08", 0);
    r = request("http://example.com/fw.mini_1", NULL, "EC800MCNLER06A08M08");
    assert(h2_pal_modem_ota_start(&f.modem.platform, &r) == H2_PAL_OK);
    h2_quectel_handle_urc_line(&f.modem, "+QIND: \"FOTA\",\"HTTPSTART\"");
    h2_quectel_handle_urc_line(&f.modem, "+QIND: \"FOTA\",\"HTTPEND\",702");
    assert(status(&f).state == H2_PAL_MODEM_OTA_UNKNOWN && f.modem.ota_hold);
    assert(h2_pal_modem_close(&f.modem.platform, 0u) == H2_PAL_ERR_BUSY);
    finish(&f);
    r = request("https://example.com/update.zip", NULL, "EC25-R02");
    init(&f, "EC25", "EC25-R01", 0);
    f.start_reply = "ERROR\r\n";
    assert(h2_pal_modem_ota_start(&f.modem.platform, &r) == H2_PAL_ERR_IO);
    assert(status(&f).state == H2_PAL_MODEM_OTA_FAILED && !f.modem.ota_hold);
    finish(&f);
    init(&f, "EC25", "EC25-R01", 0);
    f.start_result = H2_PAL_ERR_TIMEOUT; f.start_reply = "";
    assert(h2_pal_modem_ota_start(&f.modem.platform, &r) == H2_PAL_ERR_TIMEOUT);
    assert(status(&f).state == H2_PAL_MODEM_OTA_UNKNOWN && f.modem.ota_hold);
    assert(h2_pal_modem_ota_start(&f.modem.platform, &r) == H2_PAL_ERR_BUSY && f.starts == 1u);
    finish(&f);
    init(&f, "EC25", "EC25-R01", 0);
    f.interrupt = "RDY";
    assert(h2_pal_modem_ota_start(&f.modem.platform, &r) == H2_PAL_ERR_INVALID_STATE);
    assert(status(&f).state == H2_PAL_MODEM_OTA_UPDATING && f.modem.ota_hold);
    finish(&f);
    init(&f, "EC25", "EC25-R01", 0);
    assert(h2_pal_modem_ota_start(&f.modem.platform, &r) == H2_PAL_OK);
    h2_quectel_handle_urc_line(&f.modem, "+QIND: \"FOTA\",\"HTTPSTART\"");
    h2_quectel_handle_urc_line(&f.modem, "+QIND: \"FOTA\",\"HTTPEND\",702");
    assert(status(&f).state == H2_PAL_MODEM_OTA_FAILED && status(&f).vendor_code == 702 && !f.modem.ota_hold);
    finish(&f);
    init(&f, "EC25", "EC25-R01", 0);
    assert(h2_pal_modem_ota_start(&f.modem.platform, &r) == H2_PAL_OK);
    h2_quectel_handle_urc_line(&f.modem, "+QIND: \"FOTA\",\"START\"");
    h2_quectel_handle_urc_line(&f.modem, "+QIND: \"FOTA\",\"END\",507");
    assert(status(&f).state == H2_PAL_MODEM_OTA_VERIFYING && status(&f).vendor_code == 507 && f.modem.ota_hold);
    finish(&f); /* Vendor retry does not count as a second host request. */
    assert(f.starts == 1u);
}

static void test_early_exchange_failure(void) {
    fixture_t f;
    init(&f, "EC25", "EC25-R01", 0);
    h2_quectel_response_t response;
    memset(&response, 0xff, sizeof(response));
    fail_operation_lock = 1;
    assert(h2_quectel_at_exchange_timeout(&f.modem, "AT+QFOTADL=?", &response, 0, 100u) == H2_PAL_ERR_IO);
    assert(response.count == 0u && !response.ok && !response.truncated && response.lines[0][0] == '\0');
    const h2_pal_modem_ota_request_t r = request("https://example.com/fw.zip", "EC25-R01", "EC25-R02");
    f.fail_after_revision = 1;
    assert(h2_pal_modem_ota_start(&f.modem.platform, &r) == H2_PAL_ERR_IO);
    assert(!f.starts && !f.probes && !f.modem.ota_hold);
    uint32_t capabilities;
    assert(h2_pal_modem_get_capabilities(&f.modem.platform, &capabilities) == H2_PAL_OK);
    assert(capabilities & H2_PAL_MODEM_CAPABILITY_OTA);
    f.wakes = 0u; f.fail_wake_at = 2u; /* Revision succeeds; capability probe cannot wake. */
    assert(h2_pal_modem_ota_start(&f.modem.platform, &r) == H2_PAL_ERR_IO);
    assert(!f.starts && !f.probes && !f.modem.ota_hold);
    assert(h2_pal_modem_get_capabilities(&f.modem.platform, &capabilities) == H2_PAL_OK);
    assert(capabilities & H2_PAL_MODEM_CAPABILITY_OTA);
    f.fail_wake_at = 0u;
    f.fail_after_probe = 1;
    assert(h2_pal_modem_ota_start(&f.modem.platform, &r) == H2_PAL_ERR_IO);
    assert(!f.starts && f.probes == 1u && status(&f).state == H2_PAL_MODEM_OTA_UNKNOWN);
    assert(h2_pal_modem_close(&f.modem.platform, 0u) == H2_PAL_ERR_BUSY);
    finish(&f);
}

static void test_admission_and_urls(void) {
    fixture_t f;
    init(&f, "EC800M", "EC800MCNLER06A07M08", 0);
    h2_pal_modem_ota_request_t r = request("http://example.com/fw.mini_1", NULL, "EC800MCNLER06A08M08");
    f.modem.call_hold = 1u;
    assert(h2_pal_modem_ota_start(&f.modem.platform, &r) == H2_PAL_ERR_BUSY && !f.revisions);
    f.modem.call_hold = 0u; f.modem.gnss_hold = 1u;
    assert(h2_pal_modem_ota_start(&f.modem.platform, &r) == H2_PAL_ERR_BUSY && !f.revisions);
    f.modem.gnss_hold = 0u; f.modem.data_hold = 1u;
    assert(h2_pal_modem_ota_start(&f.modem.platform, &r) == H2_PAL_ERR_BUSY && !f.revisions);
    f.modem.data_hold = 0u;
    const char *invalid[] = {"file:/fw.bin", "http:///fw", "http://a/fw\"x", "http://a/fw\r\nAT", "http://a/fw x", "http://a/fw\\x", "http://a/fw#x", "http://a/fw.mini_2", "http://a/fw.zip"};
    for (size_t i = 0u; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        r.url = invalid[i];
        assert(h2_pal_modem_ota_start(&f.modem.platform, &r) == H2_PAL_ERR_INVALID_ARG && !f.starts);
    }
    char long_url[257];
    memset(long_url, 'a', sizeof(long_url)); memcpy(long_url, "http://a/", 9u);
    long_url[128] = '\0'; memcpy(long_url + 121u, ".mini_1", 7u);
    r.url = long_url;
    assert(h2_pal_modem_ota_start(&f.modem.platform, &r) == H2_PAL_OK);
    finish(&f);
    init(&f, "EC800M", "EC800MCNLER06A07M08", 0);
    long_url[128] = 'a'; long_url[129] = '\0'; memcpy(long_url + 122u, ".mini_1", 7u);
    r.url = long_url;
    assert(h2_pal_modem_ota_start(&f.modem.platform, &r) == H2_PAL_ERR_INVALID_ARG && !f.starts);
    finish(&f);
    init(&f, "EC800M", "EC800MCNLER06A07M02", 0);
    r.url = "https://example.com/fw.mini_1";
    assert(h2_pal_modem_ota_start(&f.modem.platform, &r) == H2_PAL_ERR_UNSUPPORTED);
    r.url = "ftp://example.com/fw.mini_1";
    assert(h2_pal_modem_ota_start(&f.modem.platform, &r) == H2_PAL_ERR_UNSUPPORTED);
    finish(&f);
    init(&f, "EC25", "EC25-R01", 1);
    f.raw_echo = 1;
    memset(long_url, 'a', sizeof(long_url)); memcpy(long_url, "https://a/", 10u); long_url[255] = '\0';
    r = request(long_url, NULL, "EC25-R02");
    assert(h2_pal_modem_ota_start(&f.modem.platform, &r) == H2_PAL_OK);
    assert(strlen(f.last_start) > H2_QUECTEL_LINE_MAX); /* Raw TX is not truncated to the URC limit. */
    finish(&f);
    init(&f, "EC25", "EC25-R01", 0);
    long_url[255] = 'a'; long_url[256] = '\0'; r.url = long_url;
    assert(h2_pal_modem_ota_start(&f.modem.platform, &r) == H2_PAL_ERR_INVALID_ARG && !f.starts);
    finish(&f);
    memset(&f, 0, sizeof(f)); f.model = "OTHER"; f.revision = "other";
    f.probe_reply = f.start_reply = "OK\r\n";
    const h2_quectel_modem_config_t config = {.command = command, .transport_user = &f, .sync_api = &sync_api};
    assert(h2_quectel_modem_init(&f.modem, &config) == H2_PAL_OK);
    r = request("https://example.com/fw.zip", NULL, "new");
    assert(h2_pal_modem_ota_start(&f.modem.platform, &r) == H2_PAL_ERR_CLOSED);
    assert(h2_pal_modem_open(&f.modem.platform, 0u) == H2_PAL_OK);
    assert(h2_pal_modem_ota_start(&f.modem.platform, &r) == H2_PAL_ERR_UNSUPPORTED && !f.starts);
    finish(&f);
}

int main(void) {
    test_flow("EC25", "EC25-R01", "EC25-R02", "https://example.com/fw.zip", 0);
    test_flow("EC800M", "EC800MCNLER06A07M08", "EC800MCNLER06A08M08", "http://example.com/fw.mini_1", 0);
    test_flow("EC800M", "EC800MCNLER06A07M16", "EC800MCNLER06A08M16", "https://example.com/fw.bin", 0);
    test_flow("EC25", "EC25-R01", "EC25-R02", "https://example.com/fw.zip", 1);
    test_version_guards();
    test_failures();
    test_early_exchange_failure();
    test_admission_and_urls();
    return 0;
}
