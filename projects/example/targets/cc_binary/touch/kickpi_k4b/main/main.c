#define _POSIX_C_SOURCE 200809L

#include "h2_kickpi_k4b_board.h"
#include "h2/pal/h2_pal_unsupported.h"
#include "h2_touch_smoke.h"

#include <signal.h>
#include <stdio.h>

#define H2_KICKPI_K4B_TOUCH_ACTION_BUTTON_PERIPH_ID 1001u

static volatile sig_atomic_t s_stop_requested;

typedef struct touch_periph_overlay {
  const h2_pal_periph_api_t *base;
} touch_periph_overlay_t;

static const h2_pal_periph_single_button_payload_t s_push_button = {
    .delivery = H2_PAL_BUTTON_DELIVERY_PUSH_EDGE,
};

static const h2_pal_periph_info_t s_touch_action_button = {
    .id = H2_KICKPI_K4B_TOUCH_ACTION_BUTTON_PERIPH_ID,
    .type = H2_PAL_PERIPH_TYPE_SINGLE_BUTTON,
    .name = "touch_action_button",
    .payload = &s_push_button,
    .payload_size = sizeof(s_push_button),
};

static h2_pal_result_t touch_periph_list(
    void *user,
    h2_pal_periph_type_t type_filter,
    h2_pal_periph_cb_t callback,
    void *callback_user) {
  touch_periph_overlay_t *overlay = user;
  if (overlay == NULL || overlay->base == NULL || callback == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_pal_result_t result =
      h2_pal_periph_list(overlay->base, type_filter, callback, callback_user);
  if (result != H2_PAL_OK ||
      (type_filter != H2_PAL_PERIPH_TYPE_ANY &&
       type_filter != H2_PAL_PERIPH_TYPE_SINGLE_BUTTON)) {
    return result;
  }
  return callback(callback_user, &s_touch_action_button);
}

static h2_pal_result_t touch_periph_get(
    void *user,
    h2_pal_periph_id_t periph_id,
    h2_pal_periph_info_t *out_info) {
  touch_periph_overlay_t *overlay = user;
  if (overlay == NULL || overlay->base == NULL || out_info == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (periph_id == H2_KICKPI_K4B_TOUCH_ACTION_BUTTON_PERIPH_ID) {
    *out_info = s_touch_action_button;
    return H2_PAL_OK;
  }
  return h2_pal_periph_get(overlay->base, periph_id, out_info);
}

static const h2_pal_periph_vtable_t s_touch_periph_vtable = {
    .list = touch_periph_list,
    .get = touch_periph_get,
};

static h2_pal_result_t mapper_list(
    void *user,
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
      .component_id = H2_TOUCH_SMOKE_COMPONENT_ACTION_BUTTON,
      .periph_id = H2_KICKPI_K4B_TOUCH_ACTION_BUTTON_PERIPH_ID,
  };
  return callback(callback_user, &entry);
}

static h2_pal_result_t mapper_get(
    void *user,
    h2_runtime_component_id_t component_id,
    h2_pal_periph_id_t *out_periph_id) {
  (void)user;
  if (out_periph_id == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (component_id != H2_TOUCH_SMOKE_COMPONENT_ACTION_BUTTON) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  *out_periph_id = H2_KICKPI_K4B_TOUCH_ACTION_BUTTON_PERIPH_ID;
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

static void request_stop(int signal_number) {
  (void)signal_number;
  s_stop_requested = 1;
}

static int should_stop(void *user) {
  (void)user;
  return s_stop_requested != 0;
}

int main(void) {
  struct sigaction action = {0};
  action.sa_handler = request_stop;
  (void)sigemptyset(&action.sa_mask);
  if (sigaction(SIGTERM, &action, NULL) != 0 ||
      sigaction(SIGINT, &action, NULL) != 0) {
    fprintf(stderr, "H2_TOUCH_SMOKE_FAIL stage=signal rc=%d\n",
            H2_PAL_ERR_IO);
    return 1;
  }

  const h2_kickpi_k4b_board_providers_t providers = {
      .audio = h2_pal_unsupported_audio_api(),
      .audio_decoder = h2_pal_unsupported_audio_decoder_api(),
      .video_decoder = h2_pal_unsupported_video_decoder_api(),
  };
  h2_runtime_config_t runtime_config = {0};
  touch_periph_overlay_t touch_periph_overlay = {0};
  h2_pal_periph_api_t touch_periph = {
      .user = &touch_periph_overlay,
      .vtable = &s_touch_periph_vtable,
  };
  h2_runtime_t *runtime = NULL;
  h2_pal_result_t result =
      h2_kickpi_k4b_board_runtime_config(&runtime_config, &providers);
  if (result == H2_PAL_OK) {
    touch_periph_overlay.base = runtime_config.periph;
    runtime_config.periph = &touch_periph;
    runtime_config.component_mapper = &s_mapper;
  }
  if (result == H2_PAL_OK) {
    result = h2_runtime_init(&runtime_config, &runtime);
  }
  if (result == H2_PAL_OK) {
    result = h2_runtime_input_start(runtime, NULL);
  }
  h2_pal_periph_id_t action_button_periph_id = 0u;
  if (result == H2_PAL_OK) {
    result = h2_runtime_periph_id(
        runtime, H2_TOUCH_SMOKE_COMPONENT_ACTION_BUTTON,
        &action_button_periph_id);
  }
  if (result == H2_PAL_OK &&
      action_button_periph_id !=
          H2_KICKPI_K4B_TOUCH_ACTION_BUTTON_PERIPH_ID) {
    result = H2_PAL_ERR_INVALID_STATE;
  }
  if (result == H2_PAL_OK) {
    const h2_touch_smoke_config_t config = {
        .width = H2_KICKPI_K4B_DISPLAY_WIDTH,
        .height = H2_KICKPI_K4B_DISPLAY_HEIGHT,
        .should_stop = should_stop,
        .stop_user = NULL,
    };
    fprintf(stderr,
            "H2_TOUCH_SMOKE_READY board=kickpi_k4b touch=gt9xxnew_ts\n");
    result = h2_touch_smoke_run(runtime, &config);
  }
  if (runtime != NULL) {
    h2_runtime_deinit(runtime);
  }
  if (result != H2_PAL_OK) {
    fprintf(stderr, "H2_TOUCH_SMOKE_FAIL stage=run rc=%d\n", result);
    return 1;
  }
  fprintf(stderr, "H2_TOUCH_SMOKE_STOPPED board=kickpi_k4b\n");
  return 0;
}
