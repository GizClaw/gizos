#ifndef H2_BLOOMSPEAKER_ENGINE_H
#define H2_BLOOMSPEAKER_ENGINE_H

#include "h2_bloomspeaker_controller.h"
#include "h2_runtime.h"
#include "h2_bleikcp.h"

typedef struct h2_bloomspeaker_engine h2_bloomspeaker_engine_t;
typedef h2_pal_result_t (*h2_bloomspeaker_engine_advertising_control_fn)(
    void *user);

typedef struct h2_bloomspeaker_engine_config {
  h2_bloomspeaker_engine_advertising_control_fn pause_management_advertising;
  h2_bloomspeaker_engine_advertising_control_fn resume_management_advertising;
  void *management_advertising_user;
  /* Optional non-audio consumer of the same secured pairing transport.
   * Zero/default preserves the BloomSpeaker beacon and audio behavior. */
  uint8_t product_magic;
  int (*on_session)(void *user, h2_bleikcp_t *stream, bool central,
                    bool (*should_run)(void *), void *run_user);
  void *session_user;
} h2_bloomspeaker_engine_config_t;

/* A runtime without BLE is a supported Desktop/demo configuration. */
int h2_bloomspeaker_engine_start(h2_runtime_t *runtime,
                                 h2_bloomspeaker_controller_t *controller,
                                 const h2_bloomspeaker_engine_config_t *config,
                                 h2_bloomspeaker_engine_t **out_engine);

int h2_bloomspeaker_engine_stop(h2_bloomspeaker_engine_t *engine);

#endif
