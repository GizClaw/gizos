#ifndef H2_PAL_MODEM_H
#define H2_PAL_MODEM_H

#include "h2/pal/core/h2_pal_errors.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_PAL_MODEM_APN_MAX 96u
#define H2_PAL_MODEM_OPERATOR_MAX 32u
#define H2_PAL_MODEM_IDENTITY_MAX 64u
#define H2_PAL_MODEM_PHONE_NUMBER_MAX 32u

typedef struct h2_pal_modem_api h2_pal_modem_api_t;
typedef h2_pal_modem_api_t h2_pal_modem_t;

typedef enum h2_pal_modem_capability {
    H2_PAL_MODEM_CAPABILITY_DATA = 1u << 0,
    H2_PAL_MODEM_CAPABILITY_CALL = 1u << 1,
    H2_PAL_MODEM_CAPABILITY_GNSS = 1u << 2,
    H2_PAL_MODEM_CAPABILITY_CELL_LOCATE = 1u << 3,
    H2_PAL_MODEM_CAPABILITY_LOW_POWER = 1u << 4,
    H2_PAL_MODEM_CAPABILITY_CALL_VOLUME = 1u << 5,
    /** Provider supports discovery; this does not guarantee call connectivity. */
    H2_PAL_MODEM_CAPABILITY_EMERGENCY_NUMBERS = 1u << 6,
    /** URL-based modem firmware OTA. Set only with a callable PAL OTA backend. */
    H2_PAL_MODEM_CAPABILITY_OTA = 1u << 7,
} h2_pal_modem_capability_t;

/** Modem policy is volatile; product preferences remain owned by the app. */
typedef enum h2_pal_modem_power_policy {
    H2_PAL_MODEM_POWER_POLICY_ACTIVE = 0,
    H2_PAL_MODEM_POWER_POLICY_AUTO_SLEEP = 1,
} h2_pal_modem_power_policy_t;

typedef enum h2_pal_modem_power_state {
    H2_PAL_MODEM_POWER_STATE_UNKNOWN = 0,
    H2_PAL_MODEM_POWER_STATE_ACTIVE = 1,
    H2_PAL_MODEM_POWER_STATE_ASLEEP = 2,
} h2_pal_modem_power_state_t;

typedef struct h2_pal_modem_power_status {
    h2_pal_modem_power_policy_t policy;
    h2_pal_modem_power_state_t state;
} h2_pal_modem_power_status_t;

typedef enum h2_pal_modem_sim_state {
    H2_PAL_MODEM_SIM_STATE_UNKNOWN = 0,
    H2_PAL_MODEM_SIM_STATE_ABSENT = 1,
    H2_PAL_MODEM_SIM_STATE_LOCKED = 2,
    H2_PAL_MODEM_SIM_STATE_READY = 3,
} h2_pal_modem_sim_state_t;

typedef enum h2_pal_modem_registration_state {
    H2_PAL_MODEM_REGISTRATION_UNKNOWN = 0,
    H2_PAL_MODEM_REGISTRATION_OFFLINE = 1,
    H2_PAL_MODEM_REGISTRATION_SEARCHING = 2,
    H2_PAL_MODEM_REGISTRATION_DENIED = 3,
    H2_PAL_MODEM_REGISTRATION_HOME = 4,
    H2_PAL_MODEM_REGISTRATION_ROAMING = 5,
} h2_pal_modem_registration_state_t;

typedef enum h2_pal_modem_packet_state {
    H2_PAL_MODEM_PACKET_UNKNOWN = 0,
    H2_PAL_MODEM_PACKET_DETACHED = 1,
    H2_PAL_MODEM_PACKET_ATTACHING = 2,
    H2_PAL_MODEM_PACKET_ATTACHED = 3,
    H2_PAL_MODEM_PACKET_CONNECTING = 4,
    H2_PAL_MODEM_PACKET_CONNECTED = 5,
} h2_pal_modem_packet_state_t;

typedef enum h2_pal_modem_rat {
    H2_PAL_MODEM_RAT_UNKNOWN = 0,
    H2_PAL_MODEM_RAT_GSM = 1,
    H2_PAL_MODEM_RAT_GPRS = 2,
    H2_PAL_MODEM_RAT_EDGE = 3,
    H2_PAL_MODEM_RAT_WCDMA = 4,
    H2_PAL_MODEM_RAT_HSPA = 5,
    H2_PAL_MODEM_RAT_LTE = 6,
    H2_PAL_MODEM_RAT_LTE_M = 7,
    H2_PAL_MODEM_RAT_NB_IOT = 8,
    H2_PAL_MODEM_RAT_NR5G = 9,
} h2_pal_modem_rat_t;

typedef enum h2_pal_modem_data_state {
    H2_PAL_MODEM_DATA_CLOSED = 0,
    H2_PAL_MODEM_DATA_OPENING = 1,
    H2_PAL_MODEM_DATA_OPEN = 2,
    H2_PAL_MODEM_DATA_CLOSING = 3,
} h2_pal_modem_data_state_t;

typedef enum h2_pal_modem_call_direction {
    H2_PAL_MODEM_CALL_DIRECTION_UNKNOWN = 0,
    H2_PAL_MODEM_CALL_DIRECTION_INCOMING = 1,
    H2_PAL_MODEM_CALL_DIRECTION_OUTGOING = 2,
} h2_pal_modem_call_direction_t;

typedef enum h2_pal_modem_call_state {
    H2_PAL_MODEM_CALL_STATE_IDLE = 0,
    H2_PAL_MODEM_CALL_STATE_INCOMING = 1,
    H2_PAL_MODEM_CALL_STATE_DIALING = 2,
    H2_PAL_MODEM_CALL_STATE_ALERTING = 3,
    H2_PAL_MODEM_CALL_STATE_ACTIVE = 4,
    H2_PAL_MODEM_CALL_STATE_HELD = 5,
    H2_PAL_MODEM_CALL_STATE_WAITING = 6,
    H2_PAL_MODEM_CALL_STATE_ENDED = 7,
} h2_pal_modem_call_state_t;

typedef enum h2_pal_modem_gnss_state {
    H2_PAL_MODEM_GNSS_UNSUPPORTED = 0,
    H2_PAL_MODEM_GNSS_OFF = 1,
    H2_PAL_MODEM_GNSS_ACQUIRING = 2,
    H2_PAL_MODEM_GNSS_FIXED = 3,
    H2_PAL_MODEM_GNSS_FAILED = 4,
} h2_pal_modem_gnss_state_t;

typedef struct h2_pal_modem_status {
    uint32_t capabilities;
    h2_pal_modem_sim_state_t sim;
    h2_pal_modem_registration_state_t registration;
    h2_pal_modem_packet_state_t packet;
    h2_pal_modem_rat_t rat;
} h2_pal_modem_status_t;

typedef struct h2_pal_modem_identity {
    char manufacturer[H2_PAL_MODEM_IDENTITY_MAX];
    char model[H2_PAL_MODEM_IDENTITY_MAX];
    char revision[H2_PAL_MODEM_IDENTITY_MAX];
    char imei[H2_PAL_MODEM_IDENTITY_MAX];
    char imsi[H2_PAL_MODEM_IDENTITY_MAX];
} h2_pal_modem_identity_t;

typedef struct h2_pal_modem_operator {
    char name[H2_PAL_MODEM_OPERATOR_MAX];
    h2_pal_modem_rat_t rat;
} h2_pal_modem_operator_t;

typedef struct h2_pal_modem_apn_config {
    char apn[H2_PAL_MODEM_APN_MAX];
    char username[H2_PAL_MODEM_APN_MAX];
    char password[H2_PAL_MODEM_APN_MAX];
} h2_pal_modem_apn_config_t;

typedef struct h2_pal_modem_signal {
    int32_t rssi_dbm;
    int32_t ber;
    h2_pal_modem_rat_t rat;
    uint8_t rssi_valid; /**< 1 for measured RSSI; otherwise rssi_dbm is 0 and must be ignored. */
    int32_t rsrp_dbm; /**< LTE reference signal received power in dBm. */
    uint8_t rsrp_valid; /**< 1 only for a real serving LTE cell RSRP measurement. */
} h2_pal_modem_signal_t;

typedef struct h2_pal_modem_data_status {
    h2_pal_modem_data_state_t state;
    uint32_t ip4;
    uint32_t dns1_ip4;
    uint32_t dns2_ip4;
    uint8_t ip4_valid;
    h2_pal_result_t last_error;
} h2_pal_modem_data_status_t;

typedef struct h2_pal_modem_call_request {
    char number[H2_PAL_MODEM_PHONE_NUMBER_MAX];
    uint32_t timeout_ms;
} h2_pal_modem_call_request_t;

/** SIM applicability reported by the queried table, not the current SIM state. */
typedef enum h2_pal_modem_emergency_scope {
    H2_PAL_MODEM_EMERGENCY_SCOPE_UNKNOWN = 0,
    H2_PAL_MODEM_EMERGENCY_SCOPE_WITHOUT_SIM = 1,
    H2_PAL_MODEM_EMERGENCY_SCOPE_WITH_SIM = 2,
    H2_PAL_MODEM_EMERGENCY_SCOPE_ANY = 3,
} h2_pal_modem_emergency_scope_t;

/** The table queried by the provider. MODULE does not imply that the provider
 * has also enumerated SIM files or network-supplied emergency numbers. */
typedef enum h2_pal_modem_emergency_source {
    H2_PAL_MODEM_EMERGENCY_SOURCE_UNKNOWN = 0,
    H2_PAL_MODEM_EMERGENCY_SOURCE_MODULE = 1,
    H2_PAL_MODEM_EMERGENCY_SOURCE_SIM = 2,
    H2_PAL_MODEM_EMERGENCY_SOURCE_NETWORK = 3,
} h2_pal_modem_emergency_source_t;

/** One discovered emergency number. A number may occur in multiple scopes.
 * This observation is not a promise that the network will connect a call. */
typedef struct h2_pal_modem_emergency_number {
    char number[H2_PAL_MODEM_PHONE_NUMBER_MAX]; /**< NUL-terminated decimal digits. */
    h2_pal_modem_emergency_scope_t scope;
    h2_pal_modem_emergency_source_t source;
    uint32_t categories; /**< 3GPP emergency-service category bitmap, when known. */
    uint8_t categories_valid; /**< 0 when the query does not report categories. */
} h2_pal_modem_emergency_number_t;

typedef struct h2_pal_modem_call_status {
    int32_t call_id;
    h2_pal_modem_call_direction_t direction;
    h2_pal_modem_call_state_t state;
    char number[H2_PAL_MODEM_PHONE_NUMBER_MAX];
    int32_t end_reason;
} h2_pal_modem_call_status_t;

typedef struct h2_pal_modem_call_event {
    h2_pal_modem_call_status_t call;
} h2_pal_modem_call_event_t;

typedef struct h2_pal_modem_event {
    h2_pal_result_t result;
    int32_t vendor_code;
} h2_pal_modem_event_t;

typedef struct h2_pal_modem_gnss_fix {
    uint8_t valid;
    int32_t latitude_e7;
    int32_t longitude_e7;
    int32_t altitude_cm;
    int32_t speed_cm_s;
    int32_t course_deg100;
    uint16_t hdop100;
    uint8_t satellites;
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} h2_pal_modem_gnss_fix_t;

/* Cell-based location. Independent of the GNSS path: no altitude, speed,
 * course, satellite count or UTC time, because the network service does not
 * report them. `valid == 0` means the service could not place the device and
 * is a normal result, not an error. */
typedef struct h2_pal_modem_cell_location {
    uint8_t valid;
    int32_t latitude_e7;
    int32_t longitude_e7;
    uint32_t accuracy_m; /* Horizontal accuracy reported by the service, 0 when unknown. */
} h2_pal_modem_cell_location_t;

typedef enum h2_pal_modem_ota_state {
    H2_PAL_MODEM_OTA_IDLE = 0,
    H2_PAL_MODEM_OTA_STARTING = 1,
    H2_PAL_MODEM_OTA_DOWNLOADING = 2,
    H2_PAL_MODEM_OTA_UPDATING = 3,
    H2_PAL_MODEM_OTA_VERIFYING = 4,
    H2_PAL_MODEM_OTA_SUCCEEDED = 5,
    H2_PAL_MODEM_OTA_FAILED = 6,
    H2_PAL_MODEM_OTA_UNKNOWN = 7,
} h2_pal_modem_ota_state_t;

/** Strings are borrowed for ota_start only. Revisions are opaque identifiers
 * as returned in identity.revision, compared exactly, never ordered as semver. */
typedef struct h2_pal_modem_ota_request {
    const char *url; /**< One package URL; scheme/length limits are provider-specific. */
    const char *expected_revision; /**< Optional source-version guard; NULL disables it. */
    const char *target_revision; /**< Required, nonempty; fewer than IDENTITY_MAX bytes. */
    uint32_t timeout_ms; /**< Per-command start timeout; 0 uses provider config. */
} h2_pal_modem_ota_request_t;

/** Snapshot of the last request in this provider instance; not persisted. */
typedef struct h2_pal_modem_ota_status {
    uint32_t attempt_id; /**< Nonzero request sequence, including already-at-target requests. */
    h2_pal_modem_ota_state_t state;
    uint32_t progress_percent; /**< Progress within state, only if progress_valid. */
    uint8_t progress_valid;
    h2_pal_result_t last_error;
    int32_t vendor_code; /**< Vendor terminal code; 0 when none has been reported. */
    char source_revision[H2_PAL_MODEM_IDENTITY_MAX];
    char target_revision[H2_PAL_MODEM_IDENTITY_MAX];
    char observed_revision[H2_PAL_MODEM_IDENTITY_MAX];
} h2_pal_modem_ota_status_t;

typedef struct h2_pal_modem_vtable {
    h2_pal_result_t (*open)(void *user, uint32_t timeout_ms);
    h2_pal_result_t (*close)(void *user, uint32_t timeout_ms);
    h2_pal_result_t (*get_capabilities)(void *user, uint32_t *out_capabilities);
    h2_pal_result_t (*get_status)(void *user, h2_pal_modem_status_t *out_status);
    h2_pal_result_t (*get_identity)(void *user, h2_pal_modem_identity_t *out_identity);
    h2_pal_result_t (*get_operator)(void *user, h2_pal_modem_operator_t *out_operator);
    h2_pal_result_t (*set_apn)(void *user, const h2_pal_modem_apn_config_t *config);
    h2_pal_result_t (*data_open)(void *user, uint32_t timeout_ms);
    h2_pal_result_t (*data_close)(void *user, uint32_t timeout_ms);
    h2_pal_result_t (*get_data_status)(void *user, h2_pal_modem_data_status_t *out_status);
    h2_pal_result_t (*get_signal)(void *user, h2_pal_modem_signal_t *out_signal);
    h2_pal_result_t (*call_dial)(void *user, const h2_pal_modem_call_request_t *request);
    h2_pal_result_t (*call_answer)(void *user, uint32_t timeout_ms);
    h2_pal_result_t (*call_hangup)(void *user, uint32_t timeout_ms);
    h2_pal_result_t (*get_call_status)(void *user, h2_pal_modem_call_status_t *out_status);
    /** Call speaker volume in 0..100 percent, mapped to the module's scale.
     * Both operations block in task context and serialize with other AT
     * operations. Use the modem task, never a UI/main loop or ISR. No active
     * call is required. The provider does not save or restore volume across
     * close/open; callers re-apply it when needed. A module may automatically
     * persist its volume setting (for example EC800M CLVL). */
    h2_pal_result_t (*set_call_volume)(void *user, uint32_t percent);
    h2_pal_result_t (*get_call_volume)(void *user, uint32_t *out_percent);
    h2_pal_result_t (*gnss_start)(void *user, uint32_t timeout_ms);
    h2_pal_result_t (*gnss_stop)(void *user, uint32_t timeout_ms);
    h2_pal_result_t (*get_gnss_state)(void *user, h2_pal_modem_gnss_state_t *out_state);
    h2_pal_result_t (*get_gnss_fix)(void *user, h2_pal_modem_gnss_fix_t *out_fix);
    /* Blocking single-shot query against the operator's location service. It
     * needs packet data, so it is slower than the local GNSS calls; the
     * provider never brings data up on its own and returns
     * H2_PAL_ERR_INVALID_STATE when data is not usable. `timeout_ms == 0`
     * selects the provider's configured timeout. */
    h2_pal_result_t (*cell_locate)(void *user, uint32_t timeout_ms,
                                   h2_pal_modem_cell_location_t *out_location);
    /** Blocking, task-context policy update on an open modem. AUTO_SLEEP must
     * retain registration and incoming-call reachability. The provider owns
     * wake/activity holds across complete operations and ongoing call/GNSS/data
     * sessions; it must not implicitly open data for cell_locate. Success means
     * policy accepted, not actual sleep. Close resets policy to ACTIVE.
     * Unsupported model/wiring/backend returns UNSUPPORTED. On failure the
     * last accepted policy remains; physical state may be UNKNOWN. */
    h2_pal_result_t (*set_power_policy)(void *user, h2_pal_modem_power_policy_t policy);
    /** Task-context observation into caller-owned storage, without waking the
     * modem merely to query it. Report UNKNOWN without reliable observation.
     * Fails INVALID_STATE when closed; unsupported backends return UNSUPPORTED. */
    h2_pal_result_t (*get_power_status)(void *user, h2_pal_modem_power_status_t *out_status);
    /** Read-only discovery on an open modem, blocking in task context and
     * serialized with other AT operations. Must attempt discovery without a
     * SIM-ready or registration gate; a backend can report SIM availability
     * errors. Does not open data, dial, or change stored numbers. A provider
     * that temporarily selects a phonebook must restore the previous selection
     * before success and report any restore failure.
     * Return only queried entries, never a manual's defaults or examples.
     * timeout_ms == 0 selects the provider's command timeout. An incomplete
     * response or insufficient capacity returns TRUNCATED, never a partial
     * successful list. No entries are retained across calls. */
    h2_pal_result_t (*get_emergency_numbers)(void *user, uint32_t timeout_ms,
        h2_pal_modem_emergency_number_t *out_numbers, size_t capacity,
        size_t *out_count);
    /** Start module firmware OTA on an open modem in task context. OK accepts
     * the request, not completion. Providers copy the target version, validate
     * the optional source guard and return BUSY while another OTA/session owns
     * the modem. An already-matching target may succeed without downloading.
     * A timeout can leave the outcome UNKNOWN: inspect status before retrying.
     * Keep module power, instance and URC reception alive across module resets;
     * close/deinit must reject an ongoing or uncertain update. No implicit
     * power cycle, cancellation, retry, host OTA or preference persistence. */
    h2_pal_result_t (*ota_start)(void *user, const h2_pal_modem_ota_request_t *request);
    /** Observe the last request. May perform one bounded firmware-version
     * query after the module's completion and restart notifications. SUCCEEDED
     * requires a fresh target-version match, or the target already being active
     * at start. Progress 100 and download completion alone are insufficient.
     * Missing backend returns UNSUPPORTED. Call from the modem task, not a URC
     * callback/ISR/UI loop. An in-progress snapshot can carry last_error while
     * returning OK. Output is cleared on API failure. */
    h2_pal_result_t (*ota_get_status)(void *user, uint32_t timeout_ms,
        h2_pal_modem_ota_status_t *out_status);
} h2_pal_modem_vtable_t;

struct h2_pal_modem_api {
    void *user;
    const h2_pal_modem_vtable_t *vtable;
};

static inline int h2_pal_modem_ota_revision_valid(const char *revision) {
    if (revision == NULL || revision[0] == '\0') { return 0; }
    for (size_t i = 0u; i < H2_PAL_MODEM_IDENTITY_MAX; i++) {
        if (revision[i] == '\0') { return 1; }
        if ((unsigned char)revision[i] < 0x20u || (unsigned char)revision[i] > 0x7eu) { return 0; }
    }
    return 0;
}

/** @brief Start URL OTA; see the vtable lifecycle and version-check contract. */
static inline h2_pal_result_t h2_pal_modem_ota_start(
    const h2_pal_modem_api_t *modem, const h2_pal_modem_ota_request_t *request) {
    if (request == NULL || request->url == NULL || request->url[0] == '\0' ||
        !h2_pal_modem_ota_revision_valid(request->target_revision) ||
        (request->expected_revision != NULL &&
         !h2_pal_modem_ota_revision_valid(request->expected_revision))) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (modem == NULL || modem->vtable == NULL || modem->vtable->ota_start == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return modem->vtable->ota_start(modem->user, request);
}

/** @brief Read OTA progress and confirmed version; 0 uses provider timeout. */
static inline h2_pal_result_t h2_pal_modem_ota_get_status(
    const h2_pal_modem_api_t *modem, uint32_t timeout_ms,
    h2_pal_modem_ota_status_t *out_status) {
    if (out_status == NULL) { return H2_PAL_ERR_INVALID_ARG; }
    memset(out_status, 0, sizeof(*out_status));
    if (modem == NULL || modem->vtable == NULL || modem->vtable->ota_get_status == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    h2_pal_result_t rc = modem->vtable->ota_get_status(modem->user, timeout_ms, out_status);
    if (rc != H2_PAL_OK) { memset(out_status, 0, sizeof(*out_status)); }
    return rc;
}

/** @brief Query emergency numbers and their reported applicability/source.
 * @param modem Borrowed modem API; a missing API/operation is UNSUPPORTED.
 * @param timeout_ms Command timeout in milliseconds; 0 uses provider config.
 * @param out_numbers Required caller-owned array, with at least capacity entries.
 * @param capacity Nonzero array capacity, in entries, not bytes.
 * @param out_count Required caller-owned count. Zero on every failure.
 * @return OK for a complete list (possibly empty), UNSUPPORTED for an absent
 * backend, CLOSED when not open, TRUNCATED for incomplete output, or the
 * provider's error. For valid output arguments, the array is cleared before
 * dispatch and on failure; no stale or partial list is exposed. Call from the
 * modem task, never an ISR, UI loop, or a provider notification callback.
 * Consumers must retain scope/source and invalidate any own cache on modem
 * reset, SIM changes, and relevant network changes. See the vtable contract.
 */
static inline h2_pal_result_t h2_pal_modem_get_emergency_numbers(
    const h2_pal_modem_api_t *modem, uint32_t timeout_ms,
    h2_pal_modem_emergency_number_t *out_numbers, size_t capacity,
    size_t *out_count) {
    if (out_count != NULL) { *out_count = 0u; }
    if (out_numbers == NULL || out_count == NULL || capacity == 0u ||
        capacity > SIZE_MAX / sizeof(*out_numbers)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_numbers, 0, capacity * sizeof(*out_numbers));
    if (modem == NULL || modem->vtable == NULL ||
        modem->vtable->get_emergency_numbers == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    h2_pal_result_t rc = modem->vtable->get_emergency_numbers(
        modem->user, timeout_ms, out_numbers, capacity, out_count);
    if (rc == H2_PAL_OK && *out_count > capacity) {
        rc = H2_PAL_ERR_FORMAT;
    }
    if (rc != H2_PAL_OK) {
        *out_count = 0u;
        memset(out_numbers, 0, capacity * sizeof(*out_numbers));
    }
    return rc;
}

/** @brief Set the volatile modem policy; see the vtable lifecycle contract. */
static inline h2_pal_result_t h2_pal_modem_set_power_policy(
    const h2_pal_modem_api_t *modem, h2_pal_modem_power_policy_t policy) {
    if (policy != H2_PAL_MODEM_POWER_POLICY_ACTIVE &&
        policy != H2_PAL_MODEM_POWER_POLICY_AUTO_SLEEP) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (modem == NULL || modem->vtable == NULL || modem->vtable->set_power_policy == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return modem->vtable->set_power_policy(modem->user, policy);
}

/** @brief Observe policy and actual state without causing a wakeup.
 * @param out_status Required caller-owned output, reset even on failure.
 */
static inline h2_pal_result_t h2_pal_modem_get_power_status(
    const h2_pal_modem_api_t *modem, h2_pal_modem_power_status_t *out_status) {
    if (out_status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    out_status->policy = H2_PAL_MODEM_POWER_POLICY_ACTIVE;
    out_status->state = H2_PAL_MODEM_POWER_STATE_UNKNOWN;
    if (modem == NULL || modem->vtable == NULL || modem->vtable->get_power_status == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return modem->vtable->get_power_status(modem->user, out_status);
}

static inline h2_pal_result_t h2_pal_modem_open(
    const h2_pal_modem_api_t *modem,
    uint32_t timeout_ms) {
    if (modem == NULL || modem->vtable == NULL || modem->vtable->open == NULL) {
        return modem == NULL ? H2_PAL_ERR_INVALID_ARG : H2_PAL_ERR_UNSUPPORTED;
    }
    return modem->vtable->open(modem->user, timeout_ms);
}

static inline h2_pal_result_t h2_pal_modem_close(
    const h2_pal_modem_api_t *modem,
    uint32_t timeout_ms) {
    if (modem == NULL || modem->vtable == NULL || modem->vtable->close == NULL) {
        return modem == NULL ? H2_PAL_ERR_INVALID_ARG : H2_PAL_ERR_UNSUPPORTED;
    }
    return modem->vtable->close(modem->user, timeout_ms);
}

static inline h2_pal_result_t h2_pal_modem_get_capabilities(
    const h2_pal_modem_api_t *modem,
    uint32_t *out_capabilities) {
    if (modem == NULL || out_capabilities == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (modem->vtable == NULL || modem->vtable->get_capabilities == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return modem->vtable->get_capabilities(modem->user, out_capabilities);
}

static inline h2_pal_result_t h2_pal_modem_get_status(
    const h2_pal_modem_api_t *modem,
    h2_pal_modem_status_t *out_status) {
    if (modem == NULL || out_status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (modem->vtable == NULL || modem->vtable->get_status == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return modem->vtable->get_status(modem->user, out_status);
}

static inline h2_pal_result_t h2_pal_modem_get_identity(
    const h2_pal_modem_api_t *modem,
    h2_pal_modem_identity_t *out_identity) {
    if (modem == NULL || out_identity == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (modem->vtable == NULL || modem->vtable->get_identity == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return modem->vtable->get_identity(modem->user, out_identity);
}

static inline h2_pal_result_t h2_pal_modem_get_operator(
    const h2_pal_modem_api_t *modem,
    h2_pal_modem_operator_t *out_operator) {
    if (modem == NULL || out_operator == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (modem->vtable == NULL || modem->vtable->get_operator == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return modem->vtable->get_operator(modem->user, out_operator);
}

static inline h2_pal_result_t h2_pal_modem_set_apn(
    const h2_pal_modem_api_t *modem,
    const h2_pal_modem_apn_config_t *config) {
    if (modem == NULL || config == NULL || config->apn[0] == '\0') {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (modem->vtable == NULL || modem->vtable->set_apn == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return modem->vtable->set_apn(modem->user, config);
}

static inline h2_pal_result_t h2_pal_modem_data_open(
    const h2_pal_modem_api_t *modem,
    uint32_t timeout_ms) {
    if (modem == NULL || modem->vtable == NULL || modem->vtable->data_open == NULL) {
        return modem == NULL ? H2_PAL_ERR_INVALID_ARG : H2_PAL_ERR_UNSUPPORTED;
    }
    return modem->vtable->data_open(modem->user, timeout_ms);
}

static inline h2_pal_result_t h2_pal_modem_data_close(
    const h2_pal_modem_api_t *modem,
    uint32_t timeout_ms) {
    if (modem == NULL || modem->vtable == NULL || modem->vtable->data_close == NULL) {
        return modem == NULL ? H2_PAL_ERR_INVALID_ARG : H2_PAL_ERR_UNSUPPORTED;
    }
    return modem->vtable->data_close(modem->user, timeout_ms);
}

static inline h2_pal_result_t h2_pal_modem_get_data_status(
    const h2_pal_modem_api_t *modem,
    h2_pal_modem_data_status_t *out_status) {
    if (modem == NULL || out_status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (modem->vtable == NULL || modem->vtable->get_data_status == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return modem->vtable->get_data_status(modem->user, out_status);
}

static inline h2_pal_result_t h2_pal_modem_get_signal(
    const h2_pal_modem_api_t *modem,
    h2_pal_modem_signal_t *out_signal) {
    if (modem == NULL || out_signal == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (modem->vtable == NULL || modem->vtable->get_signal == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return modem->vtable->get_signal(modem->user, out_signal);
}

static inline h2_pal_result_t h2_pal_modem_call_dial(
    const h2_pal_modem_api_t *modem,
    const h2_pal_modem_call_request_t *request) {
    if (modem == NULL || request == NULL || request->number[0] == '\0') {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (modem->vtable == NULL || modem->vtable->call_dial == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return modem->vtable->call_dial(modem->user, request);
}

static inline h2_pal_result_t h2_pal_modem_call_answer(
    const h2_pal_modem_api_t *modem,
    uint32_t timeout_ms) {
    if (modem == NULL || modem->vtable == NULL || modem->vtable->call_answer == NULL) {
        return modem == NULL ? H2_PAL_ERR_INVALID_ARG : H2_PAL_ERR_UNSUPPORTED;
    }
    return modem->vtable->call_answer(modem->user, timeout_ms);
}

static inline h2_pal_result_t h2_pal_modem_call_hangup(
    const h2_pal_modem_api_t *modem,
    uint32_t timeout_ms) {
    if (modem == NULL || modem->vtable == NULL || modem->vtable->call_hangup == NULL) {
        return modem == NULL ? H2_PAL_ERR_INVALID_ARG : H2_PAL_ERR_UNSUPPORTED;
    }
    return modem->vtable->call_hangup(modem->user, timeout_ms);
}

static inline h2_pal_result_t h2_pal_modem_get_call_status(
    const h2_pal_modem_api_t *modem,
    h2_pal_modem_call_status_t *out_status) {
    if (modem == NULL || out_status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (modem->vtable == NULL || modem->vtable->get_call_status == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return modem->vtable->get_call_status(modem->user, out_status);
}

/** @brief Set call speaker volume; see the blocking vtable contract. */
static inline h2_pal_result_t h2_pal_modem_set_call_volume(
    const h2_pal_modem_api_t *modem, uint32_t percent) {
    if (percent > 100u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (modem == NULL || modem->vtable == NULL || modem->vtable->set_call_volume == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return modem->vtable->set_call_volume(modem->user, percent);
}

/** @brief Read current call speaker volume in 0..100 percent, blocking in task context. */
static inline h2_pal_result_t h2_pal_modem_get_call_volume(
    const h2_pal_modem_api_t *modem, uint32_t *out_percent) {
    if (out_percent == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (modem == NULL || modem->vtable == NULL || modem->vtable->get_call_volume == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return modem->vtable->get_call_volume(modem->user, out_percent);
}

static inline h2_pal_result_t h2_pal_modem_gnss_start(
    const h2_pal_modem_api_t *modem,
    uint32_t timeout_ms) {
    if (modem == NULL || modem->vtable == NULL || modem->vtable->gnss_start == NULL) {
        return modem == NULL ? H2_PAL_ERR_INVALID_ARG : H2_PAL_ERR_UNSUPPORTED;
    }
    return modem->vtable->gnss_start(modem->user, timeout_ms);
}

static inline h2_pal_result_t h2_pal_modem_gnss_stop(
    const h2_pal_modem_api_t *modem,
    uint32_t timeout_ms) {
    if (modem == NULL || modem->vtable == NULL || modem->vtable->gnss_stop == NULL) {
        return modem == NULL ? H2_PAL_ERR_INVALID_ARG : H2_PAL_ERR_UNSUPPORTED;
    }
    return modem->vtable->gnss_stop(modem->user, timeout_ms);
}

static inline h2_pal_result_t h2_pal_modem_get_gnss_state(
    const h2_pal_modem_api_t *modem,
    h2_pal_modem_gnss_state_t *out_state) {
    if (modem == NULL || out_state == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (modem->vtable == NULL || modem->vtable->get_gnss_state == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return modem->vtable->get_gnss_state(modem->user, out_state);
}

static inline h2_pal_result_t h2_pal_modem_get_gnss_fix(
    const h2_pal_modem_api_t *modem,
    h2_pal_modem_gnss_fix_t *out_fix) {
    if (modem == NULL || out_fix == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (modem->vtable == NULL || modem->vtable->get_gnss_fix == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return modem->vtable->get_gnss_fix(modem->user, out_fix);
}

static inline h2_pal_result_t h2_pal_modem_cell_locate(
    const h2_pal_modem_api_t *modem,
    uint32_t timeout_ms,
    h2_pal_modem_cell_location_t *out_location) {
    if (modem == NULL || out_location == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    out_location->valid = 0u;
    out_location->latitude_e7 = 0;
    out_location->longitude_e7 = 0;
    out_location->accuracy_m = 0u;
    if (modem->vtable == NULL || modem->vtable->cell_locate == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return modem->vtable->cell_locate(modem->user, timeout_ms, out_location);
}

#ifdef __cplusplus
}
#endif

#endif
