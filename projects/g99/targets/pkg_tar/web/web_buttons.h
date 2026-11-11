#ifndef H2_G99_WEB_BUTTONS_H
#define H2_G99_WEB_BUTTONS_H

/* Browser counterpart of the G99 board keys: one PUSH_EDGE Button periph per
 * logical Main component. The tables are immutable; nothing is allocated. */

#include "h2_g99.h"
#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Periph and component tables valid for the process lifetime. */
const h2_pal_periph_api_t* h2_g99_web_periph_api(void);
const h2_runtime_component_mapper_t* h2_g99_web_component_mapper(void);

/** Maps physical board names "ai"/"boot" to HOME/SCREEN and supports
 * the logical canvas keys "home", "screen", "up", "down", "ok".
 * Returns H2_RUNTIME_COMPONENT_ID_NONE for any other name. */
h2_runtime_component_id_t h2_g99_web_component(const char* name);

/** Pushes one ordered edge of a mapped component into Runtime. Call from the
 * entry's input task only; Runtime must outlive the call. */
h2_pal_result_t h2_g99_web_button_edge(h2_runtime_t* runtime,
                                       h2_runtime_component_id_t component_id,
                                       int pressed);

#ifdef __cplusplus
}
#endif

#endif
