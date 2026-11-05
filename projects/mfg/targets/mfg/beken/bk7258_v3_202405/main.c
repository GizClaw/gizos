#include "h2_mfg.h"
#include "h2loader_board_extension.h"
#include "h2_bk7258_board.h"

int h2loader_board_extend_runtime(h2_runtime_config_t* config) {
  h2_runtime_config_t full;
  int rc = h2_bk7258_board_runtime_config(&full);
  if (rc != 0) return rc;
  /* Keep Loader's storage namespace and reboot adapter. */
  full.fs = config->fs;
  full.power = config->power;
  *config = full;
  return 0;
}

extern const h2_mfg_profile_t h2_mfg_board_profile;

int h2loader_board_extend_app(h2_runtime_t* runtime,
                              h2loader_app_config_t* config) {
  h2_mfg_t* mfg = NULL;
  int rc = h2_mfg_create(runtime, &h2_mfg_board_profile, &mfg);
  if (rc != 0) return rc;
  config->loader.mfg_required_total = h2_mfg_board_profile.total;
  config->loader.disruptive_user = mfg;
  config->loader.before_disruptive = h2_mfg_before_disruptive;
  config->command.mfg_user = mfg;
  config->command.mfg_command = h2_mfg_command;
  config->before_startup_user = mfg;
  config->before_startup = h2_mfg_run;
  config->rearm_before_startup = h2_mfg_rearm;
  config->loader.hardware_capabilities = H2_LOADER_CAPABILITIES_ALL;
  return 0;
}
