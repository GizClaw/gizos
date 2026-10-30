#ifndef H2_MFG_H
#define H2_MFG_H

#include "h2_mfg_model.h"
#include "h2_loader_command.h"
#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_mfg h2_mfg_t;

/** Runtime/profile are borrowed for the entire Loader lifetime. */
int h2_mfg_create(h2_runtime_t* runtime, const h2_mfg_profile_t* profile,
                  h2_mfg_t** out);
/** After command users stop: BUSY retains the object while run() is active
 * or a provider has not joined. Successful destruction clears *session. */
int h2_mfg_destroy(h2_mfg_t** session);
/** Loader before_startup hook. AUTO recovery bypasses interactive work.
 * The command service remains live while the operator uses this session. */
int h2_mfg_run(void* user, h2_loader_t* loader,
               const h2_pal_sync_api_t* operation_sync,
               h2_pal_mutex_t* operation_mutex,
               const h2_pal_sync_api_t* wifi_sync, h2_pal_mutex_t* wifi_mutex);
void h2_mfg_rearm(void* user);
/** Cooperative stop/join, safe with the Loader operation mutex held. Failure
 * retains every borrowed resource and prevents installation/reboot. */
int h2_mfg_before_disruptive(void* user, h2_loader_disruptive_action_t action);
int h2_mfg_command(void* user, h2_loader_command_transport_t transport,
                   size_t argc, const char* const* argv, char* output,
                   size_t capacity);

#ifdef __cplusplus
}
#endif
#endif
