#ifndef BUFFET_CHECKPOINT_H
#define BUFFET_CHECKPOINT_H

#include "h2_buffet.h"

static inline void bf_checkpoint(const h2_buffet_config_t* cfg,
                                 h2_buffet_stage_t stage) {
  if (cfg && cfg->checkpoint) cfg->checkpoint(cfg->user, stage);
}

#endif
