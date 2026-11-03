#ifndef H2_PASSPORT_RESOURCES_H
#define H2_PASSPORT_RESOURCES_H

#include "h2_mfg_model.h"
#include "h2_runtime.h"
#include "h2_loader_command.h"

int h2_passport_resources_init(h2_runtime_t* runtime);
void h2_passport_resources_bind_wifi(const h2_pal_sync_api_t* sync,
                                    h2_pal_mutex_t* mutex);
int h2_passport_prepare_check(void* user, h2_mfg_check_t check);
int h2_passport_release_check(void* user);
int h2_passport_resources_poll(void* user, h2_runtime_t* runtime,
                              const h2_loader_command_config_t* command);
int h2_passport_resources_stop(void* user, h2_runtime_t* runtime);

#endif
