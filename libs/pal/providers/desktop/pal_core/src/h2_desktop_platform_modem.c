#include "h2_desktop_platform.h"

#include <string.h>

static h2_desktop_modem_config_t s_config;
static char s_operator_name[H2_PAL_MODEM_OPERATOR_MAX];
static h2_pal_modem_data_state_t s_data_state = H2_PAL_MODEM_DATA_CLOSED;
static h2_pal_modem_call_status_t s_call_status;
static int32_t s_next_call_id = 1;
static const h2_pal_system_event_api_t *s_system_event;

static int system_event_valid(
    const h2_pal_system_event_api_t *system_event) {
  return system_event != NULL && system_event->vtable != NULL &&
         system_event->vtable->post != NULL;
}

static int32_t next_call_id(void) {
  const int32_t call_id = s_next_call_id;
  s_next_call_id =
      s_next_call_id == INT32_MAX ? 1 : s_next_call_id + 1;
  return call_id;
}

int h2_desktop_platform_inject_modem_call_event(
    h2_pal_periph_id_t source_id, h2_pal_system_event_type_t type,
    const h2_pal_modem_call_status_t *status) {
  if (source_id == 0u || status == NULL ||
      (type != H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_INCOMING &&
       type != H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_STATE_CHANGED &&
       type != H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_ENDED)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_pal_modem_call_event_t payload = {.call = *status};
  payload.call.number[sizeof(payload.call.number) - 1u] = '\0';
  s_call_status = payload.call;
  const h2_pal_system_event_t event = {
      .type = type,
      .source_id = source_id,
      .payload = &payload,
      .payload_size = sizeof(payload),
  };
  return h2_pal_system_event_post(s_system_event, &event, 0u);
}

int h2_desktop_platform_inject_incoming_call_number(
    h2_pal_periph_id_t source_id, const char *number) {
  if (number == NULL || number[0] == '\0' ||
      strlen(number) >= sizeof(s_call_status.number)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_pal_modem_call_status_t status = {
      .call_id = next_call_id(),
      .direction = H2_PAL_MODEM_CALL_DIRECTION_INCOMING,
      .state = H2_PAL_MODEM_CALL_STATE_INCOMING,
  };
  memcpy(status.number, number, strlen(number) + 1u);
  return h2_desktop_platform_inject_modem_call_event(
      source_id, H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_INCOMING, &status);
}

int h2_desktop_platform_inject_incoming_call(h2_pal_periph_id_t source_id) {
  return h2_desktop_platform_inject_incoming_call_number(source_id,
                                                          "13800000000");
}

static h2_pal_result_t post_call_state(h2_pal_modem_call_state_t state,
                                       h2_pal_system_event_type_t type) {
  s_call_status.state = state;
  const h2_pal_periph_id_t source_id =
      s_config.call_source_id != 0u ? s_config.call_source_id : 1u;
  return (h2_pal_result_t)h2_desktop_platform_inject_modem_call_event(
      source_id, type, &s_call_status);
}

int h2_desktop_platform_configure_modem(
    const h2_desktop_modem_config_t *config) {
  if (config == NULL ||
      (config->operator_name != NULL &&
       strlen(config->operator_name) >= H2_PAL_MODEM_OPERATOR_MAX) ||
      config->rat < H2_PAL_MODEM_RAT_UNKNOWN ||
      config->rat > H2_PAL_MODEM_RAT_NR5G) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  s_config = *config;
  s_operator_name[0] = '\0';
  if (config->operator_name != NULL) {
    const size_t operator_len = strlen(config->operator_name);
    memcpy(s_operator_name, config->operator_name, operator_len + 1u);
    s_config.operator_name = s_operator_name;
  }
  s_data_state = config->available && config->mobile_data_enabled
                     ? H2_PAL_MODEM_DATA_OPEN
                     : H2_PAL_MODEM_DATA_CLOSED;
  memset(&s_call_status, 0, sizeof(s_call_status));
  s_call_status.state = H2_PAL_MODEM_CALL_STATE_IDLE;
  s_next_call_id = 1;
  return H2_PAL_OK;
}

static h2_pal_result_t
h2_desktop_modem_get_capabilities(void *user, uint32_t *out_capabilities) {
  (void)user;
  if (out_capabilities == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_capabilities = s_config.available ? H2_PAL_MODEM_CAPABILITY_DATA |
                                               H2_PAL_MODEM_CAPABILITY_CALL
                                         : 0u;
  return H2_PAL_OK;
}

static h2_pal_result_t
h2_desktop_modem_get_status(void *user, h2_pal_modem_status_t *out_status) {
  (void)user;
  if (out_status == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_status, 0, sizeof(*out_status));
  if (!s_config.available) {
    return H2_PAL_OK;
  }
  out_status->capabilities =
      H2_PAL_MODEM_CAPABILITY_DATA | H2_PAL_MODEM_CAPABILITY_CALL;
  out_status->sim = H2_PAL_MODEM_SIM_STATE_READY;
  out_status->registration = H2_PAL_MODEM_REGISTRATION_HOME;
  out_status->packet = s_data_state == H2_PAL_MODEM_DATA_OPEN
                           ? H2_PAL_MODEM_PACKET_CONNECTED
                           : H2_PAL_MODEM_PACKET_ATTACHED;
  out_status->rat = s_config.rat;
  return H2_PAL_OK;
}

static h2_pal_result_t
h2_desktop_modem_get_operator(void *user,
                              h2_pal_modem_operator_t *out_operator) {
  (void)user;
  if (out_operator == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (!s_config.available)
    return H2_PAL_ERR_UNAVAILABLE;
  memset(out_operator, 0, sizeof(*out_operator));
  if (s_config.operator_name != NULL) {
    strncpy(out_operator->name, s_config.operator_name,
            sizeof(out_operator->name) - 1u);
  }
  out_operator->rat = s_config.rat;
  return H2_PAL_OK;
}

static h2_pal_result_t h2_desktop_modem_data_open(void *user,
                                                  uint32_t timeout_ms) {
  (void)user;
  (void)timeout_ms;
  if (!s_config.available)
    return H2_PAL_ERR_UNAVAILABLE;
  s_data_state = H2_PAL_MODEM_DATA_OPEN;
  return H2_PAL_OK;
}

static h2_pal_result_t h2_desktop_modem_data_close(void *user,
                                                   uint32_t timeout_ms) {
  (void)user;
  (void)timeout_ms;
  if (!s_config.available)
    return H2_PAL_ERR_UNAVAILABLE;
  s_data_state = H2_PAL_MODEM_DATA_CLOSED;
  return H2_PAL_OK;
}

static h2_pal_result_t
h2_desktop_modem_get_data_status(void *user,
                                 h2_pal_modem_data_status_t *out_status) {
  (void)user;
  if (out_status == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (!s_config.available)
    return H2_PAL_ERR_UNAVAILABLE;
  memset(out_status, 0, sizeof(*out_status));
  out_status->state = s_data_state;
  out_status->last_error = H2_PAL_OK;
  if (s_data_state == H2_PAL_MODEM_DATA_OPEN) {
    out_status->ip4 = 0x0a000002u;
    out_status->dns1_ip4 = 0x08080808u;
    out_status->ip4_valid = 1u;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t
h2_desktop_modem_get_signal(void *user, h2_pal_modem_signal_t *out_signal) {
  (void)user;
  if (out_signal == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_signal, 0, sizeof(*out_signal));
  if (!s_config.available || s_data_state != H2_PAL_MODEM_DATA_OPEN) {
    return H2_PAL_ERR_UNAVAILABLE;
  }
  *out_signal = (h2_pal_modem_signal_t){
      .rssi_dbm = s_config.rssi_dbm,
      .ber = 0,
      .rat = s_config.rat,
  };
  return H2_PAL_OK;
}

static h2_pal_result_t
h2_desktop_modem_get_call_status(void *user,
                                 h2_pal_modem_call_status_t *out_status) {
  (void)user;
  if (out_status == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_status = s_call_status;
  return H2_PAL_OK;
}

static h2_pal_result_t h2_desktop_modem_call_dial(
    void *user, const h2_pal_modem_call_request_t *request) {
  (void)user;
  if (request == NULL || request->number[0] == '\0' ||
      memchr(request->number, '\0', sizeof(request->number)) == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (!s_config.available)
    return H2_PAL_ERR_UNAVAILABLE;
  if (s_config.dial_result != H2_PAL_OK)
    return s_config.dial_result;
  memset(&s_call_status, 0, sizeof(s_call_status));
  s_call_status.call_id = next_call_id();
  s_call_status.direction = H2_PAL_MODEM_CALL_DIRECTION_OUTGOING;
  memcpy(s_call_status.number, request->number, strlen(request->number) + 1u);
  h2_pal_result_t rc = post_call_state(
      H2_PAL_MODEM_CALL_STATE_DIALING,
      H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_STATE_CHANGED);
  if (rc == H2_PAL_OK) {
    rc = post_call_state(H2_PAL_MODEM_CALL_STATE_ALERTING,
                         H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_STATE_CHANGED);
  }
  if (rc == H2_PAL_OK) {
    rc = post_call_state(H2_PAL_MODEM_CALL_STATE_ACTIVE,
                         H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_STATE_CHANGED);
  }
  return rc;
}

static h2_pal_result_t h2_desktop_modem_call_answer(void *user,
                                                    uint32_t timeout_ms) {
  (void)user;
  (void)timeout_ms;
  if (!s_config.available)
    return H2_PAL_ERR_UNAVAILABLE;
  if (s_call_status.direction != H2_PAL_MODEM_CALL_DIRECTION_INCOMING ||
      s_call_status.state != H2_PAL_MODEM_CALL_STATE_INCOMING) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  return post_call_state(H2_PAL_MODEM_CALL_STATE_ACTIVE,
                         H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_STATE_CHANGED);
}

static h2_pal_result_t h2_desktop_modem_call_hangup(void *user,
                                                    uint32_t timeout_ms) {
  (void)user;
  (void)timeout_ms;
  if (!s_config.available)
    return H2_PAL_ERR_UNAVAILABLE;
  if (s_call_status.state == H2_PAL_MODEM_CALL_STATE_IDLE ||
      s_call_status.state == H2_PAL_MODEM_CALL_STATE_ENDED) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  return post_call_state(H2_PAL_MODEM_CALL_STATE_ENDED,
                         H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_ENDED);
}

h2_pal_modem_t *h2_desktop_platform_modem(
    const h2_pal_system_event_api_t *system_event) {
  static const h2_pal_modem_vtable_t vtable = {
      .get_capabilities = h2_desktop_modem_get_capabilities,
      .get_status = h2_desktop_modem_get_status,
      .get_operator = h2_desktop_modem_get_operator,
      .data_open = h2_desktop_modem_data_open,
      .data_close = h2_desktop_modem_data_close,
      .get_data_status = h2_desktop_modem_get_data_status,
      .get_signal = h2_desktop_modem_get_signal,
      .call_dial = h2_desktop_modem_call_dial,
      .call_answer = h2_desktop_modem_call_answer,
      .call_hangup = h2_desktop_modem_call_hangup,
      .get_call_status = h2_desktop_modem_get_call_status,
  };
  static h2_pal_modem_t modem = {
      .user = NULL,
      .vtable = &vtable,
  };
  if (!system_event_valid(system_event) ||
      (s_system_event != NULL && s_system_event != system_event)) {
    return NULL;
  }
  s_system_event = system_event;
  return &modem;
}
