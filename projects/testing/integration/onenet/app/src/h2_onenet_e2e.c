#include "h2_onenet_e2e.h"

#include <string.h>

typedef struct run_state {
  h2_onenet_e2e_result_t* result;
  h2_pal_result_t error;
  bool response;
} run_state_t;

static void event(void* user, h2_onenet_t* client,
                  const h2_onenet_event_t* value) {
  run_state_t* state = user;
  if (value->type == H2_ONENET_RESPONSE) {
    ++state->result->responses;
    state->result->platform_code = value->code;
    state->response = true;
    if (state->error == H2_PAL_OK)
      state->error = value->result != H2_PAL_OK ? value->result
                     : value->code == 200       ? H2_PAL_OK
                                                : H2_PAL_ERR_UNAVAILABLE;
  } else if (value->type == H2_ONENET_COMMAND) {
    ++state->result->commands;
    state->result->last_command_operation = value->operation;
    const h2_pal_result_t rc =
        h2_onenet_reply(client, value->command_handle, 501, NULL, 0u);
    state->result->command_reply_result = rc;
    if (state->error == H2_PAL_OK) state->error = rc;
  } else if (value->type == H2_ONENET_ERROR ||
             value->type == H2_ONENET_DISCONNECTED) {
    if (state->error == H2_PAL_OK)
      state->error =
          value->result != H2_PAL_OK ? value->result : H2_PAL_ERR_CLOSED;
  }
}

static h2_pal_result_t process_until(const h2_onenet_e2e_config_t* config,
                                     h2_onenet_t* client, run_state_t* state,
                                     bool response) {
  uint64_t start = 0u;
  h2_pal_result_t rc =
      h2_pal_time_get_monotonic_ms(config->client.time, &start);
  while (rc == H2_PAL_OK && state->error == H2_PAL_OK) {
    if (response ? state->response : h2_onenet_is_connected(client)) break;
    uint64_t now = 0u;
    rc = h2_pal_time_get_monotonic_ms(config->client.time, &now);
    if (rc != H2_PAL_OK) break;
    if (now < start || now - start >= config->timeout_ms)
      return H2_PAL_ERR_TIMEOUT;
    rc = h2_onenet_process(client, 20u);
  }
  return rc != H2_PAL_OK ? rc : state->error;
}

h2_pal_result_t h2_onenet_e2e_run(const h2_onenet_e2e_config_t* config,
                                  h2_onenet_e2e_result_t* out_result) {
  if (out_result == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out_result =
      (h2_onenet_e2e_result_t){.connect_result = H2_PAL_ERR_INVALID_ARG,
                               .post_result = H2_PAL_ERR_INVALID_STATE};
  if (config == NULL || config->access_token == NULL ||
      config->params_json == NULL || config->timeout_ms == 0u ||
      config->client.time == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  run_state_t state = {.result = out_result};
  h2_onenet_config_t client_config = config->client;
  client_config.on_event = event;
  client_config.event_user = &state;
  h2_onenet_t* client = NULL;
  h2_pal_result_t rc = h2_onenet_create(&client_config, &client);
  if (rc == H2_PAL_OK) rc = h2_onenet_connect(client, config->access_token);
  if (rc == H2_PAL_OK) rc = process_until(config, client, &state, false);
  out_result->connect_result = rc;
  if (rc == H2_PAL_OK) {
    uint64_t id = 0u;
    rc = h2_onenet_publish(client, H2_ONENET_PROPERTY_POST,
                           (const uint8_t*)config->params_json,
                           strlen(config->params_json), &id);
    if (rc == H2_PAL_OK) rc = process_until(config, client, &state, true);
    out_result->post_result = rc;
  }
  if (client != NULL) {
    out_result->cleanup_result = h2_onenet_disconnect(client);
    const h2_pal_result_t destroyed = h2_onenet_destroy(&client);
    if (out_result->cleanup_result == H2_PAL_OK)
      out_result->cleanup_result = destroyed;
  }
  return rc != H2_PAL_OK ? rc : out_result->cleanup_result;
}
