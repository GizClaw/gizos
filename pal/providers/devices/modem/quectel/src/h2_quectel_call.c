#include "h2_quectel_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static h2_pal_modem_call_state_t parse_clcc_state(int state) {
  switch (state) {
    case 0:
      return H2_PAL_MODEM_CALL_STATE_ACTIVE;
    case 1:
      return H2_PAL_MODEM_CALL_STATE_HELD;
    case 2:
      return H2_PAL_MODEM_CALL_STATE_DIALING;
    case 3:
      return H2_PAL_MODEM_CALL_STATE_ALERTING;
    case 4:
      return H2_PAL_MODEM_CALL_STATE_INCOMING;
    case 5:
      return H2_PAL_MODEM_CALL_STATE_WAITING;
    default:
      return H2_PAL_MODEM_CALL_STATE_IDLE;
  }
}

int32_t h2_quectel_incoming_call_begin(h2_quectel_modem_t* modem) {
  if (modem == NULL) {
    return 0;
  }
  if (modem->incoming_call_id != 0) {
    return modem->incoming_call_id;
  }
  if (modem->next_incoming_call_id <= 0) {
    modem->next_incoming_call_id = 1;
  }
  modem->dsci_call_id = 0;
  modem->incoming_dsci_seen = 0u;
  modem->incoming_answered = 0u;
  modem->incoming_modem_call_id = 0;
  modem->incoming_call_id = modem->next_incoming_call_id;
  modem->next_incoming_call_id = modem->next_incoming_call_id == INT32_MAX
                                     ? 1
                                     : modem->next_incoming_call_id + 1;
  return modem->incoming_call_id;
}

int32_t h2_quectel_incoming_call_current(const h2_quectel_modem_t* modem) {
  return modem != NULL ? modem->incoming_call_id : 0;
}

int32_t h2_quectel_incoming_call_end(h2_quectel_modem_t* modem) {
  if (modem == NULL) {
    return 0;
  }
  const int32_t call_id = modem->incoming_call_id;
  modem->incoming_call_id = 0;
  modem->incoming_dsci_seen = 0u;
  modem->incoming_answered = 0u;
  modem->incoming_modem_call_id = 0;
  return call_id;
}

static int dial_number_valid(const char* number, size_t number_capacity,
                             size_t* out_number_len) {
  if (number == NULL || number_capacity == 0u || out_number_len == NULL) {
    return 0;
  }
  for (size_t i = 0u; i < number_capacity; ++i) {
    const unsigned char ch = (unsigned char)number[i];
    if (ch == '\0') {
      *out_number_len = i;
      return i != 0u;
    }
    if (!(h2_quectel_ascii_digit(ch) || ch == '+' || ch == '*' || ch == '#' ||
          ch == ',' || ch == 'p' || ch == 'P' || ch == 'w' || ch == 'W')) {
      return 0;
    }
  }
  return 0;
}

int h2_quectel_parse_clcc_line(const char* line,
                               h2_pal_modem_call_status_t* out_status,
                               int* out_mode) {
  if (line == NULL || out_status == NULL || out_mode == NULL) {
    return 0;
  }
  int id = 0, dir = 0, state = 0, mode = 0, mpty = 0, offset = 0;
  if (sscanf(line, "+CLCC: %d , %d , %d , %d , %d %n", &id, &dir, &state, &mode,
             &mpty, &offset) != 5 ||
      offset == 0 || id <= 0 || dir < 0 || dir > 1 || state < 0 || state > 5 ||
      mode < 0 || mode > 2 || mpty < 0 || mpty > 1 ||
      (line[offset] != '\0' && line[offset] != ',')) {
    return 0;
  }
  memset(out_status, 0, sizeof(*out_status));
  out_status->call_id = id;
  out_status->direction = dir == 0 ? H2_PAL_MODEM_CALL_DIRECTION_OUTGOING
                                   : H2_PAL_MODEM_CALL_DIRECTION_INCOMING;
  out_status->state = parse_clcc_state(state);
  const char* quote = strchr(line + offset, '"');
  if (quote != NULL) {
    h2_quectel_copy_token(out_status->number, sizeof(out_status->number),
                          quote);
  }
  *out_mode = mode;
  return 1;
}

static int32_t tracked_incoming_modem_id(const h2_quectel_modem_t* modem) {
  if (modem->incoming_modem_call_id != 0) return modem->incoming_modem_call_id;
  return modem->incoming_dsci_seen ? modem->dsci_call_id : 0;
}

typedef struct clcc_selection {
  const h2_quectel_modem_t* modem;
  h2_pal_modem_call_status_t call;
  unsigned matches;
  int rank;
  int invalid;
  int watchdog;
  int ambiguous_number;
} clcc_selection_t;

static int clcc_rank(const h2_quectel_modem_t* modem,
                     const h2_pal_modem_call_status_t* call) {
  if (modem->incoming_call_id != 0 &&
      call->direction == H2_PAL_MODEM_CALL_DIRECTION_INCOMING &&
      call->call_id == tracked_incoming_modem_id(modem))
    return 3;
  if (modem->call_status_seen &&
      modem->observed_call.state != H2_PAL_MODEM_CALL_STATE_IDLE &&
      modem->observed_call.state != H2_PAL_MODEM_CALL_STATE_ENDED &&
      modem->observed_call.direction == H2_PAL_MODEM_CALL_DIRECTION_OUTGOING &&
      call->direction == H2_PAL_MODEM_CALL_DIRECTION_OUTGOING &&
      call->call_id == modem->observed_call.call_id)
    return 3;
  return call->direction == H2_PAL_MODEM_CALL_DIRECTION_INCOMING &&
                 (call->state == H2_PAL_MODEM_CALL_STATE_INCOMING ||
                  call->state == H2_PAL_MODEM_CALL_STATE_WAITING)
             ? 2
             : 1;
}

/* Collect every row without retaining a second fixed-size call list. A
 * partial/malformed or ambiguous list must never invent a voice identity. */
static void select_clcc_voice(void* user, const char* line) {
  clcc_selection_t* selection = user;
  h2_pal_modem_call_status_t call;
  int mode;
  if (!h2_quectel_parse_clcc_line(line, &call, &mode)) {
    selection->invalid = 1;
    return;
  }
  if (mode != 0) return;
  const h2_quectel_modem_t* modem = selection->modem;
  int rank = 1;
  if (selection->watchdog) {
    if (call.direction != H2_PAL_MODEM_CALL_DIRECTION_INCOMING ||
        (modem->incoming_modem_call_id != 0 &&
         modem->incoming_modem_call_id != call.call_id))
      return;
    if (modem->incoming_modem_call_id == 0 &&
        modem->observed_call.number[0] != '\0' && call.number[0] != '\0' &&
        strcmp(modem->observed_call.number, call.number) != 0) {
      /* CLIP and CLCC may use different country-code representations. */
      selection->ambiguous_number = 1;
      return;
    }
  } else {
    rank = clcc_rank(modem, &call);
  }
  if (rank > selection->rank) {
    selection->rank = rank;
    selection->matches = 0u;
  }
  if (rank == selection->rank && selection->matches < 2u) {
    if (selection->matches == 0u) selection->call = call;
    selection->matches++;
  }
}

static void finish_observed_call(h2_quectel_modem_t* modem) {
  h2_pal_modem_call_status_t status = modem->observed_call;
  status.state = H2_PAL_MODEM_CALL_STATE_ENDED;
  h2_quectel_post_call_status(modem, H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_ENDED,
                              &status);
}

static h2_pal_result_t h2_quectel_modem_call_dial_impl(
    h2_pal_modem_t* platform, const h2_pal_modem_call_request_t* request) {
  h2_quectel_modem_t* modem = h2_quectel_from_platform(platform);
  size_t number_len = 0u;
  if (modem == NULL || request == NULL ||
      !dial_number_valid(request->number, sizeof(request->number),
                         &number_len)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  char cmd[H2_QUECTEL_LINE_MAX];
  int n = snprintf(cmd, sizeof(cmd), "ATD%s;", request->number);
  if (n <= 0 || (size_t)n >= sizeof(cmd)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  const uint32_t generation = modem->call_generation;
  h2_pal_result_t rc = h2_quectel_at_exchange(modem, cmd, NULL, 0);
  if (rc == H2_PAL_OK && generation == modem->call_generation) {
    h2_pal_modem_call_status_t status;
    memset(&status, 0, sizeof(status));
    status.call_id = -1;
    status.direction = H2_PAL_MODEM_CALL_DIRECTION_OUTGOING;
    status.state = H2_PAL_MODEM_CALL_STATE_DIALING;
    memcpy(status.number, request->number, number_len + 1u);
    h2_quectel_post_call_status(
        modem, H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_STATE_CHANGED, &status);
  }
  return rc;
}

static h2_pal_result_t h2_quectel_modem_call_answer_impl(
    h2_pal_modem_t* platform, uint32_t timeout_ms) {
  (void)timeout_ms;
  h2_quectel_modem_t* modem = h2_quectel_from_platform(platform);
  const uint32_t generation = modem != NULL ? modem->call_generation : 0u;
  h2_pal_result_t rc = modem != NULL
                           ? h2_quectel_at_exchange(modem, "ATA", NULL, 0)
                           : H2_PAL_ERR_INVALID_ARG;
  if (rc == H2_PAL_OK && generation == modem->call_generation) {
    h2_pal_modem_call_status_t status;
    memset(&status, 0, sizeof(status));
    const int32_t incoming_call_id = h2_quectel_incoming_call_current(modem);
    status.call_id = incoming_call_id != 0 ? incoming_call_id : -1;
    status.direction = incoming_call_id != 0
                           ? H2_PAL_MODEM_CALL_DIRECTION_INCOMING
                           : H2_PAL_MODEM_CALL_DIRECTION_OUTGOING;
    status.state = H2_PAL_MODEM_CALL_STATE_ACTIVE;
    h2_quectel_post_call_status(
        modem, H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_STATE_CHANGED, &status);
  }
  return rc;
}

static h2_pal_result_t h2_quectel_modem_call_hangup_impl(
    h2_pal_modem_t* platform, uint32_t timeout_ms) {
  (void)timeout_ms;
  h2_quectel_modem_t* modem = h2_quectel_from_platform(platform);
  const uint32_t generation = modem != NULL ? modem->call_generation : 0u;
  h2_pal_result_t rc = modem != NULL
                           ? h2_quectel_at_exchange(modem, "ATH", NULL, 0)
                           : H2_PAL_ERR_INVALID_ARG;
  if (rc == H2_PAL_OK && generation == modem->call_generation) {
    h2_pal_modem_call_status_t status;
    memset(&status, 0, sizeof(status));
    const int32_t incoming_call_id = h2_quectel_incoming_call_current(modem);
    status.call_id = incoming_call_id != 0 ? incoming_call_id : -1;
    status.direction = incoming_call_id != 0
                           ? H2_PAL_MODEM_CALL_DIRECTION_INCOMING
                           : H2_PAL_MODEM_CALL_DIRECTION_OUTGOING;
    status.state = H2_PAL_MODEM_CALL_STATE_ENDED;
    h2_quectel_post_call_status(
        modem, H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_ENDED, &status);
    if (incoming_call_id != 0) {
      (void)h2_quectel_incoming_call_end(modem);
    }
  }
  return rc;
}

static h2_pal_result_t h2_quectel_modem_get_call_status_impl(
    h2_pal_modem_t* platform, h2_pal_modem_call_status_t* out_status) {
  h2_quectel_modem_t* modem = h2_quectel_from_platform(platform);
  if (modem == NULL || out_status == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_status, 0, sizeof(*out_status));
  out_status->call_id = -1;
  out_status->state = H2_PAL_MODEM_CALL_STATE_IDLE;

  const uint32_t generation = modem->call_generation;
  clcc_selection_t selection = {.modem = modem};
  h2_quectel_response_t response;
  h2_pal_result_t rc = h2_quectel_at_collect(modem, "AT+CLCC", &response, 0u,
                                             select_clcc_voice, &selection);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  if (generation != modem->call_generation) {
    *out_status = modem->observed_call;
    return H2_PAL_OK;
  }
  if (response.truncated || selection.invalid || selection.matches > 1u)
    return H2_PAL_ERR_FORMAT;
  if (selection.matches == 0u) {
    if (modem->call_status_seen &&
        modem->observed_call.state != H2_PAL_MODEM_CALL_STATE_IDLE &&
        modem->observed_call.state != H2_PAL_MODEM_CALL_STATE_ENDED) {
      finish_observed_call(modem);
      *out_status = modem->observed_call;
    }
    return H2_PAL_OK;
  }
  *out_status = selection.call;
  if (out_status->direction == H2_PAL_MODEM_CALL_DIRECTION_INCOMING) {
    const int32_t modem_id = out_status->call_id;
    const int32_t previous_id = tracked_incoming_modem_id(modem);
    if (previous_id != 0 && previous_id != modem_id)
      (void)h2_quectel_incoming_call_end(modem);
    out_status->call_id = h2_quectel_incoming_call_begin(modem);
    modem->incoming_modem_call_id = modem_id;
  }
  if (out_status->state == H2_PAL_MODEM_CALL_STATE_INCOMING) {
    h2_quectel_post_call_status(
        modem, H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_INCOMING, out_status);
  } else {
    h2_quectel_post_call_status(
        modem, H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_STATE_CHANGED, out_status);
  }
  *out_status = modem->observed_call;
  return H2_PAL_OK;
}

h2_pal_result_t h2_quectel_modem_call_dial(
    h2_pal_modem_t* platform, const h2_pal_modem_call_request_t* request) {
  h2_quectel_modem_t* modem_state = h2_quectel_from_platform(platform);
  h2_pal_result_t rc = h2_quectel_operation_begin(modem_state);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  if ((modem_state->capabilities & H2_PAL_MODEM_CAPABILITY_LOW_POWER) != 0u) {
    if (modem_state->opened == 0u) {
      return h2_quectel_operation_end(modem_state, H2_PAL_ERR_CLOSED);
    }
    rc = h2_quectel_modem_prepare(modem_state);
    if (rc != H2_PAL_OK) {
      return h2_quectel_operation_end(modem_state, rc);
    }
  }
  const uint8_t saved_hold = modem_state->call_hold;
  modem_state->call_hold = 1u;
  rc = h2_quectel_modem_call_dial_impl(platform, request);
  if (rc == H2_PAL_ERR_INVALID_ARG || rc == H2_PAL_ERR_INVALID_STATE) {
    modem_state->call_hold = saved_hold;
  }
  return h2_quectel_operation_end(modem_state, rc);
}

h2_pal_result_t h2_quectel_modem_call_answer(h2_pal_modem_t* platform,
                                             uint32_t timeout_ms) {
  h2_quectel_modem_t* modem_state = h2_quectel_from_platform(platform);
  h2_pal_result_t rc = h2_quectel_operation_begin(modem_state);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  if ((modem_state->capabilities & H2_PAL_MODEM_CAPABILITY_LOW_POWER) != 0u) {
    if (modem_state->opened == 0u) {
      return h2_quectel_operation_end(modem_state, H2_PAL_ERR_CLOSED);
    }
    rc = h2_quectel_modem_prepare(modem_state);
    if (rc != H2_PAL_OK) {
      return h2_quectel_operation_end(modem_state, rc);
    }
  }
  const uint8_t saved_hold = modem_state->call_hold;
  modem_state->call_hold = 1u;
  rc = h2_quectel_modem_call_answer_impl(platform, timeout_ms);
  if (rc == H2_PAL_ERR_INVALID_ARG || rc == H2_PAL_ERR_INVALID_STATE) {
    modem_state->call_hold = saved_hold;
  }
  return h2_quectel_operation_end(modem_state, rc);
}

h2_pal_result_t h2_quectel_modem_call_hangup(h2_pal_modem_t* platform,
                                             uint32_t timeout_ms) {
  h2_quectel_modem_t* modem_state = h2_quectel_from_platform(platform);
  h2_pal_result_t rc = h2_quectel_operation_begin(modem_state);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  if ((modem_state->capabilities & H2_PAL_MODEM_CAPABILITY_LOW_POWER) != 0u) {
    if (modem_state->opened == 0u) {
      return h2_quectel_operation_end(modem_state, H2_PAL_ERR_CLOSED);
    }
    rc = h2_quectel_modem_prepare(modem_state);
    if (rc != H2_PAL_OK) {
      return h2_quectel_operation_end(modem_state, rc);
    }
  }
  rc = h2_quectel_modem_call_hangup_impl(platform, timeout_ms);
  if (rc == H2_PAL_OK &&
      (!modem_state->call_status_seen ||
       modem_state->observed_call.state == H2_PAL_MODEM_CALL_STATE_ENDED)) {
    modem_state->call_hold = 0u;
  }
  return h2_quectel_operation_end(modem_state, rc);
}

h2_pal_result_t h2_quectel_modem_get_call_status(
    h2_pal_modem_t* platform, h2_pal_modem_call_status_t* out_status) {
  h2_quectel_modem_t* modem_state = h2_quectel_from_platform(platform);
  h2_pal_result_t rc = h2_quectel_operation_begin(modem_state);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  if ((modem_state->capabilities & H2_PAL_MODEM_CAPABILITY_LOW_POWER) != 0u) {
    if (modem_state->opened == 0u) {
      return h2_quectel_operation_end(modem_state, H2_PAL_ERR_CLOSED);
    }
    rc = h2_quectel_modem_prepare(modem_state);
    if (rc != H2_PAL_OK) {
      return h2_quectel_operation_end(modem_state, rc);
    }
  }
  rc = h2_quectel_modem_get_call_status_impl(platform, out_status);
  if (rc == H2_PAL_OK) {
    modem_state->call_hold =
        out_status->state != H2_PAL_MODEM_CALL_STATE_IDLE &&
        out_status->state != H2_PAL_MODEM_CALL_STATE_ENDED;
  }
  return h2_quectel_operation_end(modem_state, rc);
}

/* Called only by the existing worker's bounded idle callback. Never wait for
 * an app operation while holding state: RX must remain able to make progress.
 */
void h2_quectel_call_watchdog(void* user) {
  h2_quectel_modem_t* modem = user;
  if (modem == NULL) {
    return;
  }
  if (modem->operation_lock != NULL &&
      h2_pal_mutex_try_lock(modem->config.sync_api, modem->operation_lock) !=
          H2_PAL_OK) {
    return;
  }
  h2_pal_result_t rc = h2_quectel_operation_begin(modem);
  if (modem->operation_lock != NULL) {
    (void)h2_pal_mutex_unlock(modem->config.sync_api, modem->operation_lock);
  }
  if (rc != H2_PAL_OK) {
    return;
  }
  if (!modem->opened || !modem->incoming_call_id || modem->incoming_dsci_seen ||
      modem->incoming_answered ||
      modem->observed_call.direction != H2_PAL_MODEM_CALL_DIRECTION_INCOMING ||
      (modem->observed_call.state != H2_PAL_MODEM_CALL_STATE_INCOMING &&
       modem->observed_call.state != H2_PAL_MODEM_CALL_STATE_WAITING)) {
    (void)h2_quectel_operation_end(modem, H2_PAL_OK);
    return;
  }
  const int32_t id = modem->incoming_call_id;
  const uint32_t generation = modem->call_generation;
  h2_quectel_response_t* response = &modem->maintenance_response;
  clcc_selection_t selection = {.modem = modem, .watchdog = 1};
  modem->call_poll_running = 1u;
  modem->call_poll_io_budget = H2_QUECTEL_RING_POLL_TIMEOUT_MS;
  rc = h2_quectel_at_collect(modem, "AT+CLCC", response,
                             H2_QUECTEL_RING_POLL_TIMEOUT_MS, select_clcc_voice,
                             &selection);
  modem->call_poll_running = 0u;
  if (rc == H2_PAL_OK && generation == modem->call_generation &&
      id == modem->incoming_call_id && !modem->incoming_dsci_seen &&
      !response->truncated && !selection.invalid) {
    /* Ambiguous/malformed lists are unknown, just like a timeout. */
    if (selection.matches == 0u && !selection.ambiguous_number) {
      finish_observed_call(modem);
    } else if (selection.matches == 1u) {
      h2_pal_modem_call_status_t status = selection.call;
      modem->incoming_modem_call_id = status.call_id;
      status.call_id = id;
      if (status.state != modem->observed_call.state ||
          (status.number[0] != '\0' &&
           strcmp(status.number, modem->observed_call.number) != 0)) {
        const h2_pal_system_event_type_t type =
            status.state == H2_PAL_MODEM_CALL_STATE_INCOMING
                ? H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_INCOMING
                : H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_STATE_CHANGED;
        h2_quectel_post_call_status(modem, type, &status);
      }
    }
  }
  memset(response, 0, sizeof(*response));
  (void)h2_quectel_operation_end(modem, H2_PAL_OK);
}

/* strtol plus explicit bounds avoids accepting overflowing or partial levels.
 */
static int parse_volume_integer(const char** cursor, int* out) {
  char* end;
  errno = 0;
  long value = strtol(*cursor, &end, 10);
  if (end == *cursor || errno == ERANGE || value < INT_MIN || value > INT_MAX) {
    return 0;
  }
  while (h2_quectel_ascii_space((unsigned char)*end)) {
    end++;
  }
  *cursor = end;
  *out = (int)value;
  return 1;
}

/* The caller owns the operation lock across preparation, probe and command. */
static h2_pal_result_t prepare_call_volume(h2_quectel_modem_t* modem) {
  if ((modem->capabilities & H2_PAL_MODEM_CAPABILITY_LOW_POWER) != 0u) {
    if (modem->opened == 0u) {
      return H2_PAL_ERR_CLOSED;
    }
    h2_pal_result_t rc = h2_quectel_modem_prepare(modem);
    if (rc != H2_PAL_OK) {
      return rc;
    }
  }
  if (modem->call_volume_range_cached) {
    return H2_PAL_OK;
  }
  h2_quectel_response_t response;
  h2_pal_result_t rc = h2_quectel_at_exchange(modem, "AT+CLVL=?", &response, 0);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  int min = 0, max = 5;
  const char* line = h2_quectel_response_find(&response, "+CLVL:");
  int valid = 0;
  if (line != NULL && !response.truncated) {
    const char* cursor = line + strlen("+CLVL:");
    while (h2_quectel_ascii_space((unsigned char)*cursor)) {
      cursor++;
    }
    if (*cursor == '(') {
      cursor++;
      if (parse_volume_integer(&cursor, &min) && *cursor == '-') {
        cursor++;
        if (parse_volume_integer(&cursor, &max) && *cursor == ')') {
          cursor++;
          while (h2_quectel_ascii_space((unsigned char)*cursor)) {
            cursor++;
          }
          valid = *cursor == '\0' && min >= 0 && max > min;
        }
      }
    }
  }
  modem->call_volume_min = valid ? min : 0;
  modem->call_volume_max = valid ? max : 5;
  modem->call_volume_range_cached = 1u;
  return H2_PAL_OK;
}

h2_pal_result_t h2_quectel_set_call_volume(void* user, uint32_t percent) {
  h2_quectel_modem_t* modem = user;
  if (percent > 100u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_pal_result_t rc = h2_quectel_operation_begin(modem);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  rc = prepare_call_volume(modem);
  if (rc == H2_PAL_OK) {
    const uint64_t span =
        (uint64_t)(modem->call_volume_max - modem->call_volume_min);
    const int level =
        modem->call_volume_min + (int)((percent * span + 50u) / 100u);
    char command[32];
    (void)snprintf(command, sizeof(command), "AT+CLVL=%d", level);
    rc = h2_quectel_at_exchange(modem, command, NULL, 0);
  }
  return h2_quectel_operation_end(modem, rc);
}

h2_pal_result_t h2_quectel_get_call_volume(void* user, uint32_t* out_percent) {
  h2_quectel_modem_t* modem = user;
  if (out_percent == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_pal_result_t rc = h2_quectel_operation_begin(modem);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  rc = prepare_call_volume(modem);
  if (rc == H2_PAL_OK) {
    h2_quectel_response_t response;
    rc = h2_quectel_at_exchange(modem, "AT+CLVL?", &response, 0);
    if (rc == H2_PAL_OK) {
      const char* line = h2_quectel_response_find(&response, "+CLVL:");
      const char* cursor = line != NULL ? line + strlen("+CLVL:") : "";
      int level;
      if (response.truncated || !parse_volume_integer(&cursor, &level) ||
          *cursor != '\0') {
        rc = H2_PAL_ERR_FORMAT;
      } else {
        if (level < modem->call_volume_min) {
          level = modem->call_volume_min;
        }
        if (level > modem->call_volume_max) {
          level = modem->call_volume_max;
        }
        const uint64_t span =
            (uint64_t)(modem->call_volume_max - modem->call_volume_min);
        *out_percent =
            (uint32_t)(((uint64_t)(level - modem->call_volume_min) * 100u +
                        span / 2u) /
                       span);
      }
    }
  }
  return h2_quectel_operation_end(modem, rc);
}
