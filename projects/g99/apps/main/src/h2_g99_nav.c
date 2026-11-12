#include "h2_g99_nav.h"

#include "h2_g99_layout.h"

#include <stddef.h>

static void start_anim(h2_g99_nav_t* nav, h2_g99_transition_t kind,
                       uint32_t duration_ms, h2_g99_route_t to_route,
                       uint8_t from_sel, uint8_t to_sel, uint64_t now_ms) {
  nav->anim = kind;
  nav->anim_start_ms = now_ms;
  nav->anim_duration_ms = duration_ms;
  nav->from_route = nav->route;
  nav->to_route = to_route;
  nav->from_sel = from_sel;
  nav->to_sel = to_sel;
}

void h2_g99_nav_init(h2_g99_nav_t* nav, uint8_t library_count,
                     uint64_t now_ms) {
  if (nav == NULL) return;
  *nav = (h2_g99_nav_t){0};
  nav->route = H2_G99_ROUTE_BOOT;
  nav->main_sel = H2_G99_MAIN_LIBRARY;
  nav->library_count = library_count;
  nav->screen_on = true;
  nav->last_input_ms = now_ms;
  nav->screen_timeout_ms = 30000u;
  start_anim(nav, H2_G99_TRANSITION_BOOT, H2_G99_BOOT_MS, H2_G99_ROUTE_MAIN,
             H2_G99_MAIN_LIBRARY, H2_G99_MAIN_LIBRARY, now_ms);
}

uint8_t h2_g99_nav_route_count(const h2_g99_nav_t* nav, h2_g99_route_t route) {
  if (nav == NULL) return 0u;
  switch (route) {
    case H2_G99_ROUTE_MAIN:
      return H2_G99_MAIN_COUNT;
    case H2_G99_ROUTE_LIBRARY:
      return nav->library_count;
    case H2_G99_ROUTE_SETTINGS:
      return H2_G99_SETTING_COUNT - 1u;
    default:
      return 0u;
  }
}

uint8_t h2_g99_nav_wheel_position(h2_g99_route_t route, uint8_t item) {
  return route == H2_G99_ROUTE_SETTINGS && item > H2_G99_SETTING_INPUT
             ? item - 1u
             : item;
}

uint8_t h2_g99_nav_wheel_item(h2_g99_route_t route, uint8_t position) {
  return route == H2_G99_ROUTE_SETTINGS && position >= H2_G99_SETTING_INPUT
             ? position + 1u
             : position;
}

bool h2_g99_nav_main_enterable(uint8_t item) {
  return item == H2_G99_MAIN_LIBRARY || item == H2_G99_MAIN_SETTINGS;
}

bool h2_g99_nav_setting_implemented(uint8_t item) {
  return item <= H2_G99_SETTING_ABOUT || item == H2_G99_SETTING_HAPTICS ||
         item == H2_G99_SETTING_STORAGE;
}

bool h2_g99_nav_setting_editable(uint8_t item) {
  return item < H2_G99_SETTING_ABOUT;
}

bool h2_g99_nav_input_ready(const h2_g99_nav_t* nav) {
  return nav != NULL && nav->route != H2_G99_ROUTE_BOOT &&
         nav->anim == H2_G99_TRANSITION_NONE;
}

uint32_t h2_g99_nav_elapsed(const h2_g99_nav_t* nav, uint64_t now_ms) {
  if (nav == NULL || nav->anim == H2_G99_TRANSITION_NONE) return 0u;
  if (now_ms <= nav->anim_start_ms) return 0u;
  const uint64_t elapsed = now_ms - nav->anim_start_ms;
  return elapsed >= nav->anim_duration_ms ? nav->anim_duration_ms
                                          : (uint32_t)elapsed;
}

float h2_g99_nav_progress(const h2_g99_nav_t* nav, uint64_t now_ms) {
  if (nav == NULL || nav->anim == H2_G99_TRANSITION_NONE ||
      nav->anim_duration_ms == 0u)
    return 1.0f;
  return (float)h2_g99_nav_elapsed(nav, now_ms) / (float)nav->anim_duration_ms;
}

static uint8_t current_sel(const h2_g99_nav_t* nav) {
  return nav->route == H2_G99_ROUTE_MAIN ? nav->main_sel : nav->sub_sel;
}

static h2_g99_nav_result_t start_rotate(h2_g99_nav_t* nav, int8_t dir,
                                        uint64_t now_ms) {
  const uint8_t count = h2_g99_nav_route_count(nav, nav->route);
  if (count < 2u) return H2_G99_NAV_IGNORED;
  const uint8_t from = current_sel(nav);
  const uint8_t position = h2_g99_nav_wheel_position(nav->route, from);
  const uint8_t to = h2_g99_nav_wheel_item(
      nav->route, (uint8_t)(((int)position + dir + count) % count));
  start_anim(nav, H2_G99_TRANSITION_ROTATE, H2_G99_ROTATE_MS, nav->route, from,
             to, now_ms);
  nav->rotate_dir = dir;
  nav->rotate_from = 0.0f;
  return H2_G99_NAV_CHANGED;
}

static h2_g99_nav_result_t start_back(h2_g99_nav_t* nav, uint64_t now_ms) {
  nav->editing = false;
  nav->text_entry = false;
  nav->edit_count = 0u;
  nav->edit_index = 0u;
  start_anim(nav, H2_G99_TRANSITION_BACK, H2_G99_PAGE_TRANSITION_MS,
             H2_G99_ROUTE_MAIN, nav->sub_sel, nav->main_sel, now_ms);
  return H2_G99_NAV_CHANGED;
}

h2_g99_nav_result_t h2_g99_nav_tick(h2_g99_nav_t* nav, uint64_t now_ms) {
  if (nav == NULL) return H2_G99_NAV_IGNORED;
  if (nav->anim != H2_G99_TRANSITION_NONE) {
    if (now_ms < nav->anim_start_ms ||
        now_ms - nav->anim_start_ms < nav->anim_duration_ms)
      return H2_G99_NAV_IGNORED;
    nav->route = nav->to_route;
    if (nav->route == H2_G99_ROUTE_MAIN)
      nav->main_sel = nav->to_sel;
    else
      nav->sub_sel = nav->to_sel;
    if (nav->anim == H2_G99_TRANSITION_BACK) nav->sub_sel = 0u;
    nav->anim = H2_G99_TRANSITION_NONE;
    nav->rotate_dir = 0;
    nav->rotate_from = 0.0f;
    nav->last_input_ms = now_ms;
    return H2_G99_NAV_CHANGED;
  }
  if (nav->screen_on && nav->screen_timeout_ms != 0u &&
      nav->route != H2_G99_ROUTE_APP && !nav->text_entry &&
      now_ms >= nav->last_input_ms &&
      now_ms - nav->last_input_ms >= nav->screen_timeout_ms) {
    nav->screen_on = false;
    return H2_G99_NAV_SCREEN_OFF;
  }
  return H2_G99_NAV_IGNORED;
}

static h2_g99_nav_result_t settings_input(h2_g99_nav_t* nav,
                                          h2_g99_action_t action,
                                          uint64_t now_ms) {
  if (nav->text_entry) {
    if (action == H2_G99_ACTION_HOME) {
      nav->text_entry = false;
      return H2_G99_NAV_TEXT_CANCEL;
    }
    return action == H2_G99_ACTION_OK ? H2_G99_NAV_TEXT_SUBMIT
                                      : H2_G99_NAV_IGNORED;
  }
  if (nav->editing) {
    switch (action) {
      case H2_G99_ACTION_HOME:
        h2_g99_nav_end_edit(nav);
        return H2_G99_NAV_EDIT_END;
      case H2_G99_ACTION_OK:
        return H2_G99_NAV_EDIT_ACTIVATE;
      case H2_G99_ACTION_UP:
      case H2_G99_ACTION_DOWN: {
        const int dir = action == H2_G99_ACTION_UP ? -1 : 1;
        if (nav->edit_count == 0u)
          return dir < 0 ? H2_G99_NAV_EDIT_INCREASE : H2_G99_NAV_EDIT_DECREASE;
        nav->edit_index =
            (uint8_t)(((int)nav->edit_index + dir + (int)nav->edit_count) %
                      (int)nav->edit_count);
        return H2_G99_NAV_EDIT_MOVED;
      }
      default:
        return H2_G99_NAV_IGNORED;
    }
  }
  switch (action) {
    case H2_G99_ACTION_HOME:
      return start_back(nav, now_ms);
    case H2_G99_ACTION_UP:
      return start_rotate(nav, -1, now_ms);
    case H2_G99_ACTION_DOWN:
      return start_rotate(nav, 1, now_ms);
    case H2_G99_ACTION_OK:
      if (!h2_g99_nav_setting_editable(nav->sub_sel)) return H2_G99_NAV_IGNORED;
      nav->editing = true;
      nav->edit_index = 0u;
      nav->edit_count = 0u;
      return H2_G99_NAV_EDIT_BEGIN;
    default:
      return H2_G99_NAV_IGNORED;
  }
}

h2_g99_nav_result_t h2_g99_nav_input(h2_g99_nav_t* nav, h2_g99_action_t action,
                                     uint64_t now_ms) {
  if (nav == NULL || nav->route == H2_G99_ROUTE_BOOT ||
      nav->anim == H2_G99_TRANSITION_BOOT)
    return H2_G99_NAV_IGNORED;
  if (!nav->screen_on) {
    /* Any key wakes the screen; the waking press does nothing else. */
    nav->screen_on = true;
    nav->last_input_ms = now_ms;
    return H2_G99_NAV_SCREEN_ON;
  }
  if (nav->anim != H2_G99_TRANSITION_NONE) return H2_G99_NAV_IGNORED;
  nav->last_input_ms = now_ms;
  if (action == H2_G99_ACTION_SCREEN) {
    nav->screen_on = false;
    return H2_G99_NAV_SCREEN_OFF;
  }
  switch (nav->route) {
    case H2_G99_ROUTE_APP:
      if (action != H2_G99_ACTION_HOME) return H2_G99_NAV_FORWARD_TO_APP;
      nav->route = H2_G99_ROUTE_LIBRARY;
      return H2_G99_NAV_EXIT_APP;
    case H2_G99_ROUTE_MAIN:
      if (action == H2_G99_ACTION_UP) return start_rotate(nav, -1, now_ms);
      if (action == H2_G99_ACTION_DOWN) return start_rotate(nav, 1, now_ms);
      if (action != H2_G99_ACTION_OK ||
          !h2_g99_nav_main_enterable(nav->main_sel))
        return H2_G99_NAV_IGNORED;
      start_anim(nav, H2_G99_TRANSITION_ENTER, H2_G99_PAGE_TRANSITION_MS,
                 nav->main_sel == H2_G99_MAIN_LIBRARY ? H2_G99_ROUTE_LIBRARY
                                                      : H2_G99_ROUTE_SETTINGS,
                 nav->main_sel, 0u, now_ms);
      return H2_G99_NAV_CHANGED;
    case H2_G99_ROUTE_LIBRARY:
      if (action == H2_G99_ACTION_HOME) return start_back(nav, now_ms);
      if (action == H2_G99_ACTION_UP) return start_rotate(nav, -1, now_ms);
      if (action == H2_G99_ACTION_DOWN) return start_rotate(nav, 1, now_ms);
      if (action != H2_G99_ACTION_OK || nav->library_count == 0u)
        return H2_G99_NAV_IGNORED;
      nav->route = H2_G99_ROUTE_APP;
      return H2_G99_NAV_LAUNCH_APP;
    case H2_G99_ROUTE_SETTINGS:
      return settings_input(nav, action, now_ms);
    default:
      return H2_G99_NAV_IGNORED;
  }
}

h2_g99_nav_result_t h2_g99_nav_rotate_from(h2_g99_nav_t* nav, int steps,
                                           float offset, uint64_t now_ms) {
  if (nav == NULL || !nav->screen_on || nav->anim != H2_G99_TRANSITION_NONE ||
      nav->text_entry)
    return H2_G99_NAV_IGNORED;
  const bool wheel_editor = nav->route == H2_G99_ROUTE_SETTINGS &&
                            (nav->sub_sel == H2_G99_SETTING_WIFI ||
                             nav->sub_sel == H2_G99_SETTING_SCREEN_TIMEOUT ||
                             nav->sub_sel == H2_G99_SETTING_VOLUME ||
                             nav->sub_sel == H2_G99_SETTING_BRIGHTNESS);
  if (nav->editing && !wheel_editor) return H2_G99_NAV_IGNORED;
  if (nav->route != H2_G99_ROUTE_MAIN && nav->route != H2_G99_ROUTE_LIBRARY &&
      nav->route != H2_G99_ROUTE_SETTINGS)
    return H2_G99_NAV_IGNORED;
  const int count = (int)h2_g99_nav_route_count(nav, nav->route);
  if (count < 2 || (steps == 0 && offset == 0.0f)) return H2_G99_NAV_IGNORED;
  if (steps > 127) steps = 127;
  if (steps < -127) steps = -127;
  nav->last_input_ms = now_ms;
  const uint8_t from = current_sel(nav);
  const uint8_t position = h2_g99_nav_wheel_position(nav->route, from);
  const uint8_t to = h2_g99_nav_wheel_item(
      nav->route, (uint8_t)((((int)position + steps) % count + count) % count));
  if (wheel_editor && steps != 0) h2_g99_nav_end_edit(nav);
  start_anim(nav, H2_G99_TRANSITION_ROTATE, H2_G99_ROTATE_MS, nav->route, from,
             to, now_ms);
  nav->rotate_dir = (int8_t)steps;
  nav->rotate_from = offset;
  return H2_G99_NAV_CHANGED;
}

bool h2_g99_nav_finish_rotate(h2_g99_nav_t* nav, uint64_t now_ms) {
  if (nav == NULL || nav->anim != H2_G99_TRANSITION_ROTATE) return false;
  nav->anim_duration_ms = 0u;
  if (now_ms < nav->anim_start_ms) nav->anim_start_ms = now_ms;
  return h2_g99_nav_tick(nav, now_ms) == H2_G99_NAV_CHANGED;
}

void h2_g99_nav_set_edit_count(h2_g99_nav_t* nav, uint8_t count,
                               uint8_t focus) {
  if (nav == NULL || !nav->editing) return;
  nav->edit_count = count;
  nav->edit_index = count == 0u ? 0u : (focus < count ? focus : count - 1u);
}

void h2_g99_nav_set_text_entry(h2_g99_nav_t* nav, bool active) {
  if (nav == NULL) return;
  nav->text_entry = active && nav->editing;
}

void h2_g99_nav_end_edit(h2_g99_nav_t* nav) {
  if (nav == NULL) return;
  nav->editing = false;
  nav->text_entry = false;
  nav->edit_count = 0u;
  nav->edit_index = 0u;
}

void h2_g99_nav_set_screen_timeout(h2_g99_nav_t* nav, uint32_t timeout_ms,
                                   uint64_t now_ms) {
  if (nav == NULL) return;
  nav->screen_timeout_ms = timeout_ms;
  nav->last_input_ms = now_ms;
}

void h2_g99_nav_observe(const h2_g99_nav_t* nav,
                        h2_g99_observation_t* observation) {
  if (nav == NULL || observation == NULL) return;
  observation->route = nav->route;
  observation->target_route =
      nav->anim == H2_G99_TRANSITION_NONE ? nav->route : nav->to_route;
  observation->transition = nav->anim;
  observation->main_selection = nav->main_sel;
  observation->sub_selection =
      nav->route == H2_G99_ROUTE_MAIN || nav->route == H2_G99_ROUTE_BOOT
          ? 0u
          : nav->sub_sel;
  observation->editing = nav->editing;
  observation->edit_index = nav->edit_index;
  observation->input_ready = h2_g99_nav_input_ready(nav);
  observation->screen_on = nav->screen_on;
}
