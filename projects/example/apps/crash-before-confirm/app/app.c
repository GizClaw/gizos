#include "h2_crash_before_confirm.h"

#include "h2/pal/core/h2_pal_errors.h"

int h2_crash_before_confirm_run(
    h2_runtime_t *runtime,
    const h2_crash_before_confirm_config_t *config) {
    if (runtime == 0 || config == 0 || config->crash == 0) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    config->crash(config->crash_user);
    return H2_PAL_ERR_IO;
}
