#ifndef H2_G99_NAV_H
#define H2_G99_NAV_H

/* Pure G99 navigation state machine: routes, wheel selections, timed
 * transitions, settings editing and screen state. It owns input gating so
 * that boot and transitions drop input instead of queueing it. */

#include "h2_g99.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_g99_action {
  H2_G99_ACTION_HOME = 0,
  H2_G99_ACTION_SCREEN,
  H2_G99_ACTION_UP,
  H2_G99_ACTION_DOWN,
  H2_G99_ACTION_OK,
} h2_g99_action_t;

typedef enum h2_g99_nav_result {
  /** Dropped: gated, screen off without wake, or no action defined. */
  H2_G99_NAV_IGNORED = 0,
  /** Route, selection or transition changed. */
  H2_G99_NAV_CHANGED,
  /** Library OK: start the selected app; route is now APP. */
  H2_G99_NAV_LAUNCH_APP,
  /** HOME inside an app: stop it; route is back on LIBRARY. */
  H2_G99_NAV_EXIT_APP,
  /** Input reaches the running app unchanged. */
  H2_G99_NAV_FORWARD_TO_APP,
  H2_G99_NAV_SCREEN_OFF,
  H2_G99_NAV_SCREEN_ON,
  /** Editing started on the current settings item. */
  H2_G99_NAV_EDIT_BEGIN,
  H2_G99_NAV_EDIT_END,
  /** Focus row moved inside a list editor. */
  H2_G99_NAV_EDIT_MOVED,
  /** Value editor: decrease or increase. */
  H2_G99_NAV_EDIT_DECREASE,
  H2_G99_NAV_EDIT_INCREASE,
  /** OK on the focused editor row. */
  H2_G99_NAV_EDIT_ACTIVATE,
  H2_G99_NAV_TEXT_CANCEL,
  H2_G99_NAV_TEXT_SUBMIT,
} h2_g99_nav_result_t;

typedef struct h2_g99_nav {
  h2_g99_route_t route;
  uint8_t main_sel;
  uint8_t sub_sel;
  uint8_t library_count;
  /* Running animation; NONE when input is accepted. */
  h2_g99_transition_t anim;
  uint64_t anim_start_ms;
  uint32_t anim_duration_ms;
  h2_g99_route_t from_route;
  h2_g99_route_t to_route;
  uint8_t from_sel;
  uint8_t to_sel;
  /* Wheel rotation: items it turns (signed; one for a key) and the offset
   * from from_sel, in items, where it starts. A key rotation starts at
   * zero; a released drag starts where the finger left the wheel. */
  int8_t rotate_dir;
  float rotate_from;
  /* Settings editing. edit_count zero means a value editor. */
  bool editing;
  uint8_t edit_index;
  uint8_t edit_count;
  bool text_entry;
  /* Screen. */
  bool screen_on;
  uint64_t last_input_ms;
  uint32_t screen_timeout_ms;
} h2_g99_nav_t;

/** Starts the boot animation at now_ms; it ends in MAIN(Library). */
void h2_g99_nav_init(h2_g99_nav_t* nav, uint8_t library_count, uint64_t now_ms);

/** Completes a finished animation and applies the screen timeout. Returns
 * CHANGED, SCREEN_OFF or IGNORED. */
h2_g99_nav_result_t h2_g99_nav_tick(h2_g99_nav_t* nav, uint64_t now_ms);

h2_g99_nav_result_t h2_g99_nav_input(h2_g99_nav_t* nav, h2_g99_action_t action,
                                     uint64_t now_ms);

/** Ends a wheel drag that left the wheel `offset` items past the selection:
 * turns `steps` items from there, or settles back for zero steps. Accepted
 * only on a wheel route at rest, with the screen on and nothing edited. */
h2_g99_nav_result_t h2_g99_nav_rotate_from(h2_g99_nav_t* nav, int steps,
                                           float offset, uint64_t now_ms);

/** Completes a running wheel rotation at once, so a finger can catch the
 * turning wheel. Returns true when one was running. */
bool h2_g99_nav_finish_rotate(h2_g99_nav_t* nav, uint64_t now_ms);

/** True when route input is accepted: no boot or transition is running. */
bool h2_g99_nav_input_ready(const h2_g99_nav_t* nav);

/** Animation progress in [0, 1]; one when idle. */
float h2_g99_nav_progress(const h2_g99_nav_t* nav, uint64_t now_ms);

/** Elapsed milliseconds of the running animation, capped at its duration. */
uint32_t h2_g99_nav_elapsed(const h2_g99_nav_t* nav, uint64_t now_ms);

/** Number of wheel items on a route. */
uint8_t h2_g99_nav_route_count(const h2_g99_nav_t* nav, h2_g99_route_t route);

/** Whether OK can enter or edit the given item. */
bool h2_g99_nav_main_enterable(uint8_t item);
bool h2_g99_nav_setting_implemented(uint8_t item);
/** Implemented items that open an editor on OK. */
bool h2_g99_nav_setting_editable(uint8_t item);

/** Sets the list editor size (zero = value editor) and clamps the focus. */
void h2_g99_nav_set_edit_count(h2_g99_nav_t* nav, uint8_t count, uint8_t focus);

/** Enters or leaves password entry while editing Wi-Fi. */
void h2_g99_nav_set_text_entry(h2_g99_nav_t* nav, bool active);

/** Ends editing (for example after a successful connect). */
void h2_g99_nav_end_edit(h2_g99_nav_t* nav);

void h2_g99_nav_set_screen_timeout(h2_g99_nav_t* nav, uint32_t timeout_ms,
                                   uint64_t now_ms);

/** Fills the navigation fields of an observation. */
void h2_g99_nav_observe(const h2_g99_nav_t* nav,
                        h2_g99_observation_t* observation);

#ifdef __cplusplus
}
#endif

#endif
