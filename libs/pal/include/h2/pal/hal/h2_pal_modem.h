#ifndef H2_PAL_MODEM_H
#define H2_PAL_MODEM_H

#include "h2/pal/core/h2_pal_errors.h"

#include <stddef.h>
#include <stdint.h>

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
} h2_pal_modem_capability_t;

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
    h2_pal_result_t (*gnss_start)(void *user, uint32_t timeout_ms);
    h2_pal_result_t (*gnss_stop)(void *user, uint32_t timeout_ms);
    h2_pal_result_t (*get_gnss_state)(void *user, h2_pal_modem_gnss_state_t *out_state);
    h2_pal_result_t (*get_gnss_fix)(void *user, h2_pal_modem_gnss_fix_t *out_fix);
} h2_pal_modem_vtable_t;

struct h2_pal_modem_api {
    void *user;
    const h2_pal_modem_vtable_t *vtable;
};

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

#ifdef __cplusplus
}
#endif

#endif
