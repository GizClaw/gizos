#ifndef QI_DUEL_LINK_H
#define QI_DUEL_LINK_H
#include "h2_runtime.h"
#include "h2_bloomspeaker_controller.h"
#include "h2_bloomspeaker_engine.h"
#include <stdatomic.h>
typedef struct qi_duel_link {
  h2_runtime_t *runtime;
  h2_bloomspeaker_controller_t controller;
  h2_bloomspeaker_engine_t *engine;
  h2_pal_queue_t *tx,*rx;
  h2_pal_result_t (*pause_management_advertising)(void *);
  h2_pal_result_t (*resume_management_advertising)(void *);
  void *management_advertising_user;
  _Atomic int connected,central;
} qi_duel_link_t;
int qi_duel_link_open(void *state,void *user);
int qi_duel_link_stop(qi_duel_link_t *link);
#endif
