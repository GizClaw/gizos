#ifndef H2_H106_MFG_ENTRY_H
#define H2_H106_MFG_ENTRY_H

#include "h2_device_service.h"
#include "h2_mfg.h"

#include <stdio.h>

extern const h2_mfg_profile_t h2_mfg_board_profile;

/* These three H106 BSPs expose the physical FM175xx reader as peripheral 201.
 * The shared MFG contract uses a logical component, independent of that ID. */
static h2_pal_result_t nfc_mapping_list(
    void* user, h2_runtime_component_t filter,
    h2_runtime_component_mapping_cb_t callback, void* callback_user) {
  (void)user;
  if (callback == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (filter != H2_RUNTIME_COMPONENT_NONE &&
      filter != H2_RUNTIME_COMPONENT_NFC_READER)
    return H2_PAL_OK;
  const h2_runtime_component_mapping_entry_t entry = {
      .component_id = H2_MFG_NFC_COMPONENT_ID, .periph_id = 201u};
  return callback(callback_user, &entry);
}

static h2_pal_result_t nfc_mapping_get(void* user,
                                       h2_runtime_component_id_t component,
                                       h2_pal_periph_id_t* out_peripheral) {
  (void)user;
  if (out_peripheral == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out_peripheral = 0;
  if (component != H2_MFG_NFC_COMPONENT_ID) return H2_PAL_ERR_NOT_FOUND;
  *out_peripheral = 201u;
  return H2_PAL_OK;
}

static const h2_runtime_component_mapper_vtable_t nfc_mapping_vtable = {
    .list = nfc_mapping_list, .get_periph_id = nfc_mapping_get};
static const h2_runtime_component_mapper_t nfc_mapper = {
    .vtable = &nfc_mapping_vtable};

typedef struct h106_mfg_context {
  h2_runtime_t* runtime;
  h2_mfg_t* session;
  int input_started;
} h106_mfg_context_t;

static int h106_mfg_stop_input(h106_mfg_context_t* context) {
  if (!context->input_started) return H2_PAL_OK;
  const int rc = h2_runtime_input_stop(context->runtime);
  if (rc == H2_PAL_OK) context->input_started = 0;
  return rc;
}

static int h106_mfg_command(void* user, h2_loader_command_transport_t transport,
                            size_t argc, const char* const* argv, char* output,
                            size_t capacity) {
  h106_mfg_context_t* context = user;
  return h2_mfg_command(context->session, transport, argc, argv, output,
                        capacity);
}

static int h106_mfg_before_disruptive(void* user,
                                      h2_loader_disruptive_action_t action) {
  h106_mfg_context_t* context = user;
  const int rc = h2_mfg_before_disruptive(context->session, action);
  /* Loader owns its operation mutex here. Join acquisition after MFG has
   * released its hardware, before reboot can leave an I2C transfer pending. */
  return rc == H2_PAL_OK ? h106_mfg_stop_input(context) : rc;
}

static void h106_mfg_rearm(void* user) {
  h106_mfg_context_t* context = user;
  h2_mfg_rearm(context->session);
}

static int h106_mfg_run_session(void* user, h2_loader_t* loader,
                                const h2_pal_sync_api_t* operation_sync,
                                h2_pal_mutex_t* operation_mutex,
                                const h2_pal_sync_api_t* wifi_sync,
                                h2_pal_mutex_t* wifi_mutex) {
  h106_mfg_context_t* context = user;
  int rc = h2_pal_mutex_lock(operation_sync, operation_mutex);
  if (rc != H2_PAL_OK) return rc;
  if (!context->input_started) {
    rc = h2_runtime_input_start(context->runtime, NULL);
    if (rc == H2_PAL_OK) context->input_started = 1;
  }
  int unlock_rc = h2_pal_mutex_unlock(operation_sync, operation_mutex);
  if (rc != H2_PAL_OK) return rc;
  if (unlock_rc != H2_PAL_OK) return unlock_rc;

  rc = h2_mfg_run(context->session, loader, operation_sync, operation_mutex,
                  wifi_sync, wifi_mutex);
  const int lock_rc = h2_pal_mutex_lock(operation_sync, operation_mutex);
  if (lock_rc != H2_PAL_OK) return lock_rc;
  const int stop_rc = h106_mfg_stop_input(context);
  unlock_rc = h2_pal_mutex_unlock(operation_sync, operation_mutex);
  if (stop_rc != H2_PAL_OK) return stop_rc;
  return unlock_rc == H2_PAL_OK ? rc : unlock_rc;
}

/* The entry task and all configuration borrows remain alive through blocking
 * Loader execution and every cleanup retry. Input starts only once Loader has
 * finished its initial relay/install processing and enters MFG; it is joined
 * before MFG returns control for another image operation. The board task
 * wrapper releases its providers only after this function returns. */
static void h106_mfg_run(h2_runtime_config_t* config,
                         h2_device_loader_config_t* loader, int with_ble) {
  static h2_device_loader_t device;
  h2_runtime_t* runtime = NULL;
  h2_mfg_t* mfg = NULL;
  config->component_mapper = &nfc_mapper;
  int rc = h2_runtime_init(config, &runtime);
  if (rc == H2_PAL_OK) rc = h2_mfg_create(runtime, &h2_mfg_board_profile, &mfg);
  h106_mfg_context_t context = {.runtime = runtime, .session = mfg};
  if (rc == H2_PAL_OK) {
    loader->board = h2_mfg_board_profile.board;
    loader->mfg_required_total = h2_mfg_board_profile.total;
    loader->user = &context;
    loader->mfg_command = h106_mfg_command;
    loader->run_mfg = h106_mfg_run_session;
    loader->rearm_mfg = h106_mfg_rearm;
    loader->before_disruptive = h106_mfg_before_disruptive;
    rc = h2_device_loader_run(&device, runtime, loader, NULL, with_ble);
  }
  printf("H2_MFG_EXIT board=%s code=%d\n", h2_mfg_board_profile.board, rc);

  /* Device Loader has joined command users. A failed stop/join must keep the
   * Runtime, session and board APIs borrowed until cleanup actually succeeds.
   */
  while (mfg != NULL) {
    rc = h2_mfg_destroy(&mfg);
    if (rc == H2_PAL_OK) break;
    printf("H2_MFG_CLEANUP stage=session code=%d\n", rc);
    (void)h2_pal_time_sleep_ms(config->time, 50u);
  }
  while (runtime != NULL) {
    rc = h2_runtime_destroy(&runtime);
    if (rc != H2_PAL_OK) printf("H2_MFG_CLEANUP stage=runtime code=%d\n", rc);
    if (runtime != NULL) (void)h2_pal_time_sleep_ms(config->time, 50u);
  }
}

#endif
