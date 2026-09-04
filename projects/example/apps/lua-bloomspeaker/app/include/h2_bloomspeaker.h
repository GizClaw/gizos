#ifndef H2_BLOOMSPEAKER_H
#define H2_BLOOMSPEAKER_H

/** @file h2_bloomspeaker.h @brief Portable particle intercom UI. */

#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define H2_BLOOMSPEAKER_COMPONENT_POWER 6u
#define H2_BLOOMSPEAKER_COMPONENT_PAIR 7u

typedef int (*h2_bloomspeaker_should_stop_fn)(void *user);
typedef h2_pal_result_t (*h2_bloomspeaker_ready_fn)(void *user);
typedef h2_pal_result_t (*h2_bloomspeaker_advertising_control_fn)(void *user);

typedef struct h2_bloomspeaker_config {
  h2_runtime_component_id_t back_component_id;
  h2_runtime_component_id_t power_component_id;
  h2_runtime_component_id_t pairing_component_id;
  h2_bloomspeaker_should_stop_fn should_stop;
  void *should_stop_user;
  h2_bloomspeaker_ready_fn on_ready;
  void *on_ready_user;
  h2_bloomspeaker_advertising_control_fn pause_management_advertising;
  h2_bloomspeaker_advertising_control_fn resume_management_advertising;
  void *management_advertising_user;
} h2_bloomspeaker_config_t;

h2_pal_result_t h2_bloomspeaker_run(
    h2_runtime_t *runtime,
    const h2_bloomspeaker_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
