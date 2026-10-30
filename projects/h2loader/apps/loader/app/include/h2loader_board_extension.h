#ifndef H2LOADER_BOARD_EXTENSION_H
#define H2LOADER_BOARD_EXTENSION_H

#include "h2loader_app.h"

#ifdef __cplusplus
extern "C" {
#endif
/* BK/JieLi launchers supply weak no-op defaults. A board factory entry may
 * override these at link time, reusing the maintained update transports.
 * Called before Runtime init and before Loader/command services are exposed.
 * The App hook may set MFG policy, command and lifecycle callbacks; the BK
 * launcher consumes those fields while retaining its native storage flow. */
int h2loader_board_extend_runtime(h2_runtime_config_t* config);
int h2loader_board_extend_app(h2_runtime_t* runtime,
                              h2loader_app_config_t* config);

#ifdef __cplusplus
}
#endif
#endif
