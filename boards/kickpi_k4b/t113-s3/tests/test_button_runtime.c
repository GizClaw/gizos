#define _POSIX_C_SOURCE 200809L

#include "h2_kickpi_k4b_board.h"
#include "h2/pal/h2_pal_unsupported.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

static volatile sig_atomic_t s_stop_requested;

static void request_stop(int signal_number) {
  (void)signal_number;
  s_stop_requested = 1;
}

static h2_pal_result_t mapper_list(void *user,
                                   h2_runtime_component_t component_filter,
                                   h2_runtime_component_mapping_cb_t callback,
                                   void *callback_user) {
  (void)user;
  if (callback == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (component_filter != H2_RUNTIME_COMPONENT_NONE &&
      component_filter != H2_RUNTIME_COMPONENT_BUTTON) {
    return H2_PAL_OK;
  }
  const h2_runtime_component_mapping_entry_t entry = {
      .component_id = 1u,
      .periph_id = H2_KICKPI_K4B_PERIPH_ACTION_BUTTON,
  };
  return callback(callback_user, &entry);
}

static h2_pal_result_t mapper_get(void *user,
                                  h2_runtime_component_id_t component_id,
                                  h2_pal_periph_id_t *out_periph_id) {
  (void)user;
  if (out_periph_id == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (component_id != 1u) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  *out_periph_id = H2_KICKPI_K4B_PERIPH_ACTION_BUTTON;
  return H2_PAL_OK;
}

static const h2_runtime_component_mapper_vtable_t s_mapper_vtable = {
    .list = mapper_list,
    .get_periph_id = mapper_get,
};

static const h2_runtime_component_mapper_t s_mapper = {
    .user = NULL,
    .vtable = &s_mapper_vtable,
};

static h2_pal_result_t wait_for_released_baseline(
    const h2_runtime_config_t *config) {
  unsigned int stable_samples = 0u;
  for (unsigned int sample = 0u; sample < 250u && !s_stop_requested;
       ++sample) {
    h2_pal_single_button_reading_t reading = {0};
    h2_pal_result_t result = h2_pal_button_read_single_button(
        config->button, H2_KICKPI_K4B_PERIPH_ACTION_BUTTON, &reading);
    if (result != H2_PAL_OK) {
      return result;
    }
    stable_samples = reading.state == H2_PAL_BUTTON_STATE_RELEASED
                         ? stable_samples + 1u
                         : 0u;
    if (stable_samples >= 10u) {
      return H2_PAL_OK;
    }
    result = h2_pal_time_sleep_ms(config->time,
                                  H2_RUNTIME_BUTTON_POLL_INTERVAL_MS);
    if (result != H2_PAL_OK) {
      return result;
    }
  }
  return s_stop_requested ? H2_PAL_EXIT : H2_PAL_ERR_TIMEOUT;
}

static const char *event_name(h2_runtime_event_kind_t kind) {
  switch (kind) {
  case H2_RUNTIME_COMPONENT_EVENT_BUTTON_DOWN:
    return "DOWN";
  case H2_RUNTIME_COMPONENT_EVENT_BUTTON_UP:
    return "UP";
  case H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION:
    return "ACTION";
  default:
    return NULL;
  }
}

int main(void) {
  struct sigaction action = {0};
  action.sa_handler = request_stop;
  (void)sigemptyset(&action.sa_mask);
  if (sigaction(SIGTERM, &action, NULL) != 0 ||
      sigaction(SIGINT, &action, NULL) != 0) {
    fprintf(stderr, "H2_BUTTON_RUNTIME_FAIL stage=signal rc=%d\n",
            H2_PAL_ERR_IO);
    return 1;
  }

  const h2_kickpi_k4b_board_providers_t providers = {
      .audio = h2_pal_unsupported_audio_api(),
      .audio_decoder = h2_pal_unsupported_audio_decoder_api(),
      .video_decoder = h2_pal_unsupported_video_decoder_api(),
  };
  h2_runtime_config_t config = {0};
  h2_runtime_t *runtime = NULL;
  h2_pal_result_t result =
      h2_kickpi_k4b_board_runtime_config(&config, &providers);
  config.component_mapper = &s_mapper;
  config.event_queue_capacity = H2_RUNTIME_DEFAULT_EVENT_QUEUE_CAPACITY;
  if (result == H2_PAL_OK) {
    result = wait_for_released_baseline(&config);
  }
  if (result != H2_PAL_OK) {
    fprintf(stderr, "H2_BUTTON_RUNTIME_FAIL stage=baseline rc=%d\n", result);
    return 1;
  }
  result = h2_runtime_init(&config, &runtime);
  if (result == H2_PAL_OK) {
    result = h2_runtime_input_start(runtime, NULL);
  }
  h2_pal_periph_id_t periph_id = 0u;
  if (result == H2_PAL_OK) {
    result = h2_runtime_periph_id(runtime, 1u, &periph_id);
  }
  if (result != H2_PAL_OK || periph_id != H2_KICKPI_K4B_PERIPH_ACTION_BUTTON) {
    fprintf(stderr, "H2_BUTTON_RUNTIME_FAIL stage=init rc=%d periph=%u\n",
            result, (unsigned int)periph_id);
    h2_runtime_deinit(runtime);
    return 1;
  }

  fprintf(stderr,
          "H2_BUTTON_RUNTIME_READY chip=pio line=PD14(110) active_low=1\n");
  int saw_down = 0;
  int saw_up = 0;
  int saw_action = 0;
  while (!s_stop_requested && !saw_action) {
    uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    h2_runtime_event_t event = {
        .payload = payload,
        .payload_capacity = sizeof(payload),
    };
    h2_pal_result_t event_result = h2_runtime_wait_event(runtime, &event, 100u);
    if (event_result == H2_PAL_OK) {
      if (event.kind == H2_RUNTIME_COMPONENT_EVENT_ERROR) {
        h2_pal_result_t input_error = H2_PAL_ERR_INVALID_STATE;
        if (event.payload_size == sizeof(input_error)) {
          memcpy(&input_error, event.payload, sizeof(input_error));
        }
        fprintf(stderr,
                "H2_BUTTON_RUNTIME_INPUT_ERROR component=%u rc=%d\n",
                (unsigned int)event.component_id, input_error);
        result = input_error;
        break;
      }
      const char *name = event_name(event.kind);
      if (name != NULL) {
        fprintf(stderr,
                "H2_BUTTON_RUNTIME_EVENT kind=%s component=%u sequence=%llu\n",
                name, (unsigned int)event.component_id,
                (unsigned long long)event.sequence);
      }
      saw_down |= event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_DOWN;
      saw_up |= event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_UP;
      saw_action |= event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION;
    }
    if (result != H2_PAL_OK) {
      break;
    }
    if (event_result != H2_PAL_ERR_WOULD_BLOCK &&
        event_result != H2_PAL_ERR_TIMEOUT) {
      result = event_result;
      break;
    }
  }

  h2_runtime_deinit(runtime);
  if (result != H2_PAL_OK || !saw_down || !saw_up || !saw_action) {
    fprintf(stderr,
            "H2_BUTTON_RUNTIME_FAIL stage=poll rc=%d down=%d up=%d action=%d\n",
            result, saw_down, saw_up, saw_action);
    return 1;
  }
  fprintf(stderr, "H2_BUTTON_RUNTIME_PASS down=1 up=1 action=1\n");
  return 0;
}
