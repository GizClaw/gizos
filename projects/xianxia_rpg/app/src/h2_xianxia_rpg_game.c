#include "h2_xianxia_rpg_game.h"

#include "h2_xianxia_rpg_story.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define STEP_SLICE_MS 50u
#define MARKER_MS 600u
#define VICTORY_TAP_MS 450u
#define ENDING_TAP_MS 1500u
#define FLED_RETURN_MS 1400u
#define ENCOUNTER_MIN_STEPS 9u
#define ENCOUNTER_SPAN_STEPS 9u
#define DROP_PERCENT 35u
#define CRIT_PERCENT 12u
#define FLEE_PERCENT 65u
#define GUARD_MP 4u
#define LINE_PX H2_XIANXIA_RPG_LINE_PX

/* ---- Random numbers and text ------------------------------------------ */

static uint32_t next_random(h2_xianxia_rpg_game_t* game) {
  uint32_t x = game->rng;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  game->rng = x != 0u ? x : 0x9E3779B9u;
  return game->rng;
}

static uint32_t random_below(h2_xianxia_rpg_game_t* game, uint32_t bound) {
  return bound != 0u ? next_random(game) % bound : 0u;
}

static float random_unit(h2_xianxia_rpg_game_t* game) {
  return (float)(next_random(game) >> 8) / 16777216.0f;
}

uint32_t h2_xianxia_rpg_utf8_next(const char** text) {
  const unsigned char* s = (const unsigned char*)*text;
  if (s[0] == 0u) return 0u;
  uint32_t cp;
  size_t length;
  if (s[0] < 0x80u) {
    cp = s[0];
    length = 1;
  } else if ((s[0] & 0xE0u) == 0xC0u) {
    cp = s[0] & 0x1Fu;
    length = 2;
  } else if ((s[0] & 0xF0u) == 0xE0u) {
    cp = s[0] & 0x0Fu;
    length = 3;
  } else if ((s[0] & 0xF8u) == 0xF0u) {
    cp = s[0] & 0x07u;
    length = 4;
  } else {
    *text += 1;
    return 0xFFFDu;
  }
  for (size_t i = 1; i < length; ++i) {
    if ((s[i] & 0xC0u) != 0x80u) {
      *text += i;
      return 0xFFFDu;
    }
    cp = (cp << 6) | (s[i] & 0x3Fu);
  }
  *text += length;
  return cp;
}

uint32_t h2_xianxia_rpg_utf8_length(const char* text) {
  uint32_t count = 0;
  if (text == NULL) return 0;
  while (h2_xianxia_rpg_utf8_next(&text) != 0u) ++count;
  return count;
}

/* ---- Progression ------------------------------------------------------- */

uint32_t h2_xianxia_rpg_exp_for_level(uint32_t level) {
  return level <= 1u ? 0u : 15u * (level - 1u) * level / 2u;
}

void h2_xianxia_rpg_hero_at_level(h2_xianxia_rpg_hero_t* hero, uint32_t level) {
  if (level < 1u) level = 1u;
  if (level > H2_XIANXIA_RPG_LEVEL_MAX) level = H2_XIANXIA_RPG_LEVEL_MAX;
  const uint32_t up = level - 1u;
  memset(hero, 0, sizeof(*hero));
  hero->level = level;
  hero->exp = h2_xianxia_rpg_exp_for_level(level);
  hero->hp_max = 60u + 14u * up;
  hero->mp_max = 20u + 4u * up;
  hero->attack = 12u + 3u * up;
  hero->defense = 5u + 2u * up;
  hero->hp = hero->hp_max;
  hero->mp = hero->mp_max;
}

static void restore(h2_xianxia_rpg_hero_t* hero) {
  hero->hp = hero->hp_max;
  hero->mp = hero->mp_max;
}

/* ---- Layout ------------------------------------------------------------ */

static h2_xianxia_rpg_rect_t rect(int32_t x, int32_t y, int32_t w, int32_t h) {
  return (h2_xianxia_rpg_rect_t){x, y, w, h};
}

static bool inside(const h2_xianxia_rpg_rect_t* r, float x, float y) {
  return x >= (float)r->x && y >= (float)r->y && x < (float)(r->x + r->w) &&
         y < (float)(r->y + r->h);
}

static void compute_layout(h2_xianxia_rpg_game_t* game) {
  h2_xianxia_rpg_layout_t* l = &game->layout;
  const int32_t w = game->width, h = game->height;
  const int32_t pad = 8, gap = 4;
  memset(l, 0, sizeof(*l));
  l->dialog_lines = 3;
  const int32_t dialog_h = (l->dialog_lines + 1) * LINE_PX + 2 * pad;
  const int32_t stacked_hud_h = 3 * LINE_PX + 12;
  const int32_t strip_hud_h = 2 * LINE_PX + 12;

  /* Portrait screens fit HUD, map and dialogue one above another. */
  int32_t tile = w / H2_XIANXIA_RPG_MAP_COLS;
  const int32_t stacked_need =
      stacked_hud_h + H2_XIANXIA_RPG_MAP_ROWS * tile + dialog_h + 2 * gap;
  l->stacked = stacked_need <= h;
  if (l->stacked) {
    const int32_t map_h = H2_XIANXIA_RPG_MAP_ROWS * tile;
    const int32_t top = (h - stacked_need) / 2;
    l->hud = rect(0, top, w, stacked_hud_h);
    l->map_y = top + stacked_hud_h + gap;
    l->dialog = rect(pad, l->map_y + map_h + gap, w - 2 * pad, dialog_h);
  } else {
    l->hud = rect(0, 0, w, strip_hud_h);
    const int32_t by_height = (h - strip_hud_h) / H2_XIANXIA_RPG_MAP_ROWS;
    if (by_height < tile) tile = by_height;
    const int32_t map_h = H2_XIANXIA_RPG_MAP_ROWS * tile;
    l->map_y = strip_hud_h + (h - strip_hud_h - map_h) / 2;
    l->dialog = rect(pad, h - dialog_h - pad, w - 2 * pad, dialog_h);
  }
  if (tile < 8) tile = 8;
  l->tile = tile;
  l->map_x = (w - H2_XIANXIA_RPG_MAP_COLS * tile) / 2;

  const int32_t button_w = w * 6 / 10 < 280 ? w * 6 / 10 : 280;
  const int32_t button_h = h < 360 ? 40 : 48;
  const int32_t button_gap = h < 360 ? 8 : 14;
  int32_t button_y = h * 60 / 100;
  const int32_t buttons_end =
      button_y + H2_XIANXIA_RPG_TITLE_BUTTONS * button_h + button_gap;
  if (buttons_end > h - 8) button_y -= buttons_end - (h - 8);
  for (int32_t i = 0; i < H2_XIANXIA_RPG_TITLE_BUTTONS; ++i)
    l->title_buttons[i] =
        rect((w - button_w) / 2, button_y + i * (button_h + button_gap),
             button_w, button_h);

  const int32_t message_h = 2 * LINE_PX + 2 * pad;
  const int32_t command_h = 46;
  const int32_t panel_h = message_h + command_h + 3 * pad;
  l->stage = rect(0, 0, w, h - panel_h);
  l->panel = rect(0, h - panel_h, w, panel_h);
  l->message = rect(pad, l->panel.y + pad, w - 2 * pad, message_h);
  const int32_t command_w =
      (w - 2 * pad - (H2_XIANXIA_RPG_COMMANDS - 1) * pad) /
      H2_XIANXIA_RPG_COMMANDS;
  for (int32_t i = 0; i < H2_XIANXIA_RPG_COMMANDS; ++i)
    l->commands[i] = rect(pad + i * (command_w + pad),
                          l->message.y + message_h + pad, command_w, command_h);
}

/* ---- Map queries ------------------------------------------------------- */

static bool npc_at(const h2_xianxia_rpg_game_t* game, int32_t x, int32_t y,
                   h2_xianxia_rpg_npc_t* out) {
  h2_xianxia_rpg_npc_t npcs[H2_XIANXIA_RPG_MAX_NPCS];
  const size_t count = h2_xianxia_rpg_npcs(game, game->map, npcs);
  for (size_t i = 0; i < count; ++i) {
    if (npcs[i].x == x && npcs[i].y == y) {
      if (out != NULL) *out = npcs[i];
      return true;
    }
  }
  return false;
}

bool h2_xianxia_rpg_walkable(const h2_xianxia_rpg_game_t* game,
                             h2_xianxia_rpg_map_t map, int32_t x, int32_t y) {
  switch (h2_xianxia_rpg_tile(map, x, y)) {
    case '.':
    case ',':
    case '=':
    case 'b':
    case '_':
    case 's':
      return true;
    case 'X':
      return game->stage >= H2_XIANXIA_RPG_STAGE_TO_SHRINE;
    default:
      return false;
  }
}

/* Tiles the hero may use: walkable and not occupied by a character. */
static bool passable(const h2_xianxia_rpg_game_t* game, int32_t x, int32_t y) {
  return h2_xianxia_rpg_walkable(game, game->map, x, y) &&
         !npc_at(game, x, y, NULL);
}

static bool interactive(const h2_xianxia_rpg_game_t* game, int32_t x,
                        int32_t y) {
  if (npc_at(game, x, y, NULL)) return true;
  const char tile = h2_xianxia_rpg_tile(game->map, x, y);
  if (tile == 'W' || tile == 'S') return true;
  return tile == 'X' && game->stage < H2_XIANXIA_RPG_STAGE_TO_SHRINE;
}

/* Breadth-first path from the hero tile to the goal, or to any passable
 * neighbor of it when adjacent is true. Replaces the current path. */
static bool find_path(h2_xianxia_rpg_game_t* game, int32_t goal_x,
                      int32_t goal_y, bool adjacent) {
  enum { CELLS = H2_XIANXIA_RPG_PATH_MAX };
  int16_t previous[CELLS];
  uint8_t queue[CELLS];
  for (int32_t i = 0; i < CELLS; ++i) previous[i] = -2;
  const int32_t start = game->ty * H2_XIANXIA_RPG_MAP_COLS + game->tx;
  previous[start] = -1;
  size_t head = 0, tail = 0;
  queue[tail++] = (uint8_t)start;
  int32_t found = -1;
  static const int32_t dx[4] = {0, 0, -1, 1}, dy[4] = {1, -1, 0, 0};
  while (head < tail) {
    const int32_t cell = queue[head++];
    const int32_t x = cell % H2_XIANXIA_RPG_MAP_COLS;
    const int32_t y = cell / H2_XIANXIA_RPG_MAP_COLS;
    const int32_t manhattan = (x > goal_x ? x - goal_x : goal_x - x) +
                              (y > goal_y ? y - goal_y : goal_y - y);
    if (adjacent ? manhattan == 1 : manhattan == 0) {
      found = cell;
      break;
    }
    for (int32_t d = 0; d < 4; ++d) {
      const int32_t nx = x + dx[d], ny = y + dy[d];
      if (nx < 0 || ny < 0 || nx >= H2_XIANXIA_RPG_MAP_COLS ||
          ny >= H2_XIANXIA_RPG_MAP_ROWS)
        continue;
      const int32_t next = ny * H2_XIANXIA_RPG_MAP_COLS + nx;
      if (previous[next] != -2 || !passable(game, nx, ny)) continue;
      previous[next] = (int16_t)cell;
      queue[tail++] = (uint8_t)next;
    }
  }
  if (found < 0) return false;
  uint32_t length = 0;
  for (int32_t cell = found; cell != start; cell = previous[cell]) ++length;
  game->path_len = length;
  game->path_pos = 0;
  for (int32_t cell = found; cell != start; cell = previous[cell]) {
    --length;
    game->path_x[length] = (uint8_t)(cell % H2_XIANXIA_RPG_MAP_COLS);
    game->path_y[length] = (uint8_t)(cell / H2_XIANXIA_RPG_MAP_COLS);
  }
  return true;
}

static void clear_path(h2_xianxia_rpg_game_t* game) {
  game->path_len = game->path_pos = 0;
  game->target_x = game->target_y = -1;
}

/* ---- Particles --------------------------------------------------------- */

static void particle_spawn(h2_xianxia_rpg_game_t* game,
                           h2_xianxia_rpg_particle_t* p, uint8_t kind,
                           const h2_xianxia_rpg_rect_t* area, bool anywhere) {
  memset(p, 0, sizeof(*p));
  p->active = true;
  p->kind = kind;
  p->x = (float)area->x + random_unit(game) * (float)area->w;
  p->y = anywhere ? (float)area->y + random_unit(game) * (float)area->h
                  : (float)area->y - 4.0f;
  p->phase = random_unit(game) * 2.0f * H2_XIANXIA_RPG_PI;
  p->life_ms = 6000u + random_below(game, 6000u);
  const float u = game->unit;
  switch (kind) {
    case 0: /* bamboo leaf */
      p->vx = (8.0f + 10.0f * random_unit(game)) * u;
      p->vy = (22.0f + 14.0f * random_unit(game)) * u;
      break;
    case 1: /* petal */
      p->vx = (10.0f + 14.0f * random_unit(game)) * u;
      p->vy = (16.0f + 12.0f * random_unit(game)) * u;
      break;
    default: /* firefly */
      if (!anywhere) p->y = (float)area->y + random_unit(game) * (float)area->h;
      p->vx = (random_unit(game) - 0.5f) * 16.0f * u;
      p->vy = (random_unit(game) - 0.5f) * 12.0f * u;
      break;
  }
}

static void particles_reset(h2_xianxia_rpg_game_t* game) {
  memset(game->particles, 0, sizeof(game->particles));
  uint32_t count = 0;
  uint8_t kind = 1;
  h2_xianxia_rpg_rect_t area = rect(0, 0, game->width, game->height);
  if (game->phase == H2_XIANXIA_RPG_PHASE_EXPLORE) {
    const h2_xianxia_rpg_layout_t* l = &game->layout;
    area = rect(l->map_x, l->map_y, H2_XIANXIA_RPG_MAP_COLS * l->tile,
                H2_XIANXIA_RPG_MAP_ROWS * l->tile);
    switch (game->map) {
      case H2_XIANXIA_RPG_MAP_FOREST:
        kind = 0;
        count = 6;
        break;
      case H2_XIANXIA_RPG_MAP_SHRINE:
        kind = 2;
        count = 7;
        break;
      default:
        kind = 1;
        count = 4;
        break;
    }
  } else if (game->phase == H2_XIANXIA_RPG_PHASE_TITLE ||
             game->phase == H2_XIANXIA_RPG_PHASE_ENDING) {
    count = 10;
  }
  for (uint32_t i = 0; i < count && i < H2_XIANXIA_RPG_MAX_PARTICLES; ++i)
    particle_spawn(game, &game->particles[i], kind, &area, true);
}

static void particles_step(h2_xianxia_rpg_game_t* game, uint32_t dt_ms) {
  const float dt = (float)dt_ms / 1000.0f;
  h2_xianxia_rpg_rect_t area = rect(0, 0, game->width, game->height);
  if (game->phase == H2_XIANXIA_RPG_PHASE_EXPLORE) {
    const h2_xianxia_rpg_layout_t* l = &game->layout;
    area = rect(l->map_x, l->map_y, H2_XIANXIA_RPG_MAP_COLS * l->tile,
                H2_XIANXIA_RPG_MAP_ROWS * l->tile);
  }
  for (unsigned i = 0; i < H2_XIANXIA_RPG_MAX_PARTICLES; ++i) {
    h2_xianxia_rpg_particle_t* p = &game->particles[i];
    if (!p->active) continue;
    p->age_ms += dt_ms;
    p->phase += dt * (p->kind == 2 ? 2.2f : 1.6f);
    if (p->kind == 2) {
      p->vx += (random_unit(game) - 0.5f) * 30.0f * game->unit * dt;
      p->vy += (random_unit(game) - 0.5f) * 30.0f * game->unit * dt;
    }
    p->x += p->vx * dt;
    p->y += p->vy * dt;
    const bool out =
        p->x < (float)area.x - 8.0f || p->x > (float)(area.x + area.w) + 8.0f ||
        p->y < (float)area.y - 8.0f || p->y > (float)(area.y + area.h) + 8.0f;
    if (out || p->age_ms >= p->life_ms)
      particle_spawn(game, p, p->kind, &area, p->kind == 2);
  }
}

/* ---- Phases ------------------------------------------------------------ */

static void enter_phase(h2_xianxia_rpg_game_t* game,
                        h2_xianxia_rpg_phase_t phase) {
  game->phase = phase;
  game->phase_started_ms = game->time_ms;
  particles_reset(game);
}

static void reset_encounter(h2_xianxia_rpg_game_t* game) {
  game->encounter_steps =
      ENCOUNTER_MIN_STEPS + random_below(game, ENCOUNTER_SPAN_STEPS);
}

static void place_hero(h2_xianxia_rpg_game_t* game, h2_xianxia_rpg_map_t map,
                       int32_t x, int32_t y) {
  game->map = map;
  game->tx = game->from_x = x;
  game->ty = game->from_y = y;
  game->moving = false;
  game->move_ms = 0;
  clear_path(game);
  game->banner_ms = H2_XIANXIA_RPG_BANNER_MS;
  game->marker_ms = 0;
  reset_encounter(game);
}

static void start_dialogue(h2_xianxia_rpg_game_t* game,
                           h2_xianxia_rpg_script_id_t id) {
  const h2_xianxia_rpg_script_t* script = h2_xianxia_rpg_script(id);
  if (script == NULL || script->count == 0u) return;
  h2_xianxia_rpg_dialogue_t* d = &game->dialogue;
  memset(d, 0, sizeof(*d));
  d->active = true;
  d->script = script;
  d->total = h2_xianxia_rpg_utf8_length(script->lines[0].text);
}

static void request_save(h2_xianxia_rpg_game_t* game) {
  game->save_pending = true;
}

static void advance_stage(h2_xianxia_rpg_game_t* game,
                          h2_xianxia_rpg_stage_t stage) {
  game->stage = stage;
  game->objective_ms = H2_XIANXIA_RPG_OBJECTIVE_MS;
}

/* ---- Battle ------------------------------------------------------------ */

static void battle_say(h2_xianxia_rpg_game_t* game, const char* format, ...) {
  h2_xianxia_rpg_battle_t* b = &game->battle;
  va_list args;
  va_start(args, format);
  (void)vsnprintf(b->message, sizeof(b->message), format, args);
  va_end(args);
  ++b->message_id;
}

static void battle_append(h2_xianxia_rpg_game_t* game, const char* format,
                          ...) {
  h2_xianxia_rpg_battle_t* b = &game->battle;
  const size_t used = strlen(b->message);
  if (used + 1u >= sizeof(b->message)) return;
  va_list args;
  va_start(args, format);
  (void)vsnprintf(b->message + used, sizeof(b->message) - used, format, args);
  va_end(args);
  ++b->message_id;
}

static const char* text(h2_xianxia_rpg_text_id_t id) {
  return h2_xianxia_rpg_text[id];
}

static void start_battle(h2_xianxia_rpg_game_t* game,
                         h2_xianxia_rpg_enemy_t enemy) {
  h2_xianxia_rpg_battle_t* b = &game->battle;
  const h2_xianxia_rpg_enemy_info_t* info = h2_xianxia_rpg_enemy_info(enemy);
  const uint32_t message_id = b->message_id;
  memset(b, 0, sizeof(*b));
  b->message_id = message_id;
  b->enemy = (uint8_t)enemy;
  b->boss = enemy == H2_XIANXIA_RPG_ENEMY_DRAGON;
  b->enemy_hp = b->enemy_hp_max = info->hp;
  b->enemy_attack = info->attack;
  b->enemy_defense = info->defense;
  b->state = H2_XIANXIA_RPG_BATTLE_INTRO;
  battle_say(game,
             text(b->boss ? H2_XIANXIA_RPG_TEXT_BOSS_APPEARS
                          : H2_XIANXIA_RPG_TEXT_APPEARS),
             info->name);
  game->moving = false;
  game->from_x = game->tx;
  game->from_y = game->ty;
  clear_path(game);
  game->dialogue.active = false;
  enter_phase(game, H2_XIANXIA_RPG_PHASE_BATTLE);
}

static void set_battle_state(h2_xianxia_rpg_game_t* game,
                             h2_xianxia_rpg_battle_state_t state) {
  game->battle.state = state;
  game->battle.state_ms = 0;
}

static void popup(h2_xianxia_rpg_game_t* game, bool on_enemy, bool heal,
                  uint32_t value, int32_t slot) {
  h2_xianxia_rpg_battle_t* b = &game->battle;
  h2_xianxia_rpg_popup_t* chosen = &b->popups[0];
  for (unsigned i = 0; i < H2_XIANXIA_RPG_MAX_POPUPS; ++i) {
    if (!b->popups[i].active) {
      chosen = &b->popups[i];
      break;
    }
    if (b->popups[i].age_ms > chosen->age_ms) chosen = &b->popups[i];
  }
  *chosen = (h2_xianxia_rpg_popup_t){.active = true,
                                     .on_enemy = on_enemy,
                                     .heal = heal,
                                     .value = value,
                                     .slot = slot};
}

/* Damage after defense with +/- spread percent; never below 1. */
static uint32_t roll_damage(h2_xianxia_rpg_game_t* game, uint32_t attack,
                            uint32_t multiplier_percent, uint32_t spread,
                            uint32_t defense) {
  const uint32_t roll = 100u - spread + random_below(game, 2u * spread + 1u);
  const uint32_t raw = attack * multiplier_percent * roll / 10000u;
  const uint32_t armor = defense / 2u;
  return raw > armor + 1u ? raw - armor : 1u;
}

bool h2_xianxia_rpg_command_available(const h2_xianxia_rpg_game_t* game,
                                      h2_xianxia_rpg_command_t command) {
  switch (command) {
    case H2_XIANXIA_RPG_COMMAND_SKILL:
      return game->stage >= H2_XIANXIA_RPG_STAGE_TO_SHRINE &&
             game->hero.mp >= H2_XIANXIA_RPG_SKILL_MP;
    case H2_XIANXIA_RPG_COMMAND_PILL:
      return game->hero.pills > 0u;
    case H2_XIANXIA_RPG_COMMAND_FLEE:
      return !game->battle.boss;
    case H2_XIANXIA_RPG_COMMAND_ATTACK:
    case H2_XIANXIA_RPG_COMMAND_GUARD:
      return true;
    default:
      return false;
  }
}

static void enemy_turn(h2_xianxia_rpg_game_t* game) {
  h2_xianxia_rpg_battle_t* b = &game->battle;
  const h2_xianxia_rpg_enemy_info_t* info =
      h2_xianxia_rpg_enemy_info((h2_xianxia_rpg_enemy_t)b->enemy);
  set_battle_state(game, H2_XIANXIA_RPG_BATTLE_ENEMY_ACT);
  b->hit_count = b->hits_applied = 0;
  b->hit_on_enemy = false;
  b->special = false;
  b->message[0] = '\0';
  ++b->message_id;
  if (b->boss && !b->enraged && b->enemy_hp * 10u < b->enemy_hp_max * 4u) {
    b->enraged = true;
    b->enemy_attack += 4u;
    battle_append(game, "%s", text(H2_XIANXIA_RPG_TEXT_ENRAGE));
  }
  uint32_t multiplier = 100u;
  if (b->boss && b->charging) {
    b->charging = false;
    b->special = true;
    multiplier = 220u;
  } else if (b->boss &&
             b->turn % (b->enraged ? 2u : 3u) == (b->enraged ? 1u : 2u)) {
    b->charging = true;
    battle_append(game, "%s", text(H2_XIANXIA_RPG_TEXT_CHARGE));
    return;
  } else if (!b->boss && random_below(game, 100u) < 25u) {
    b->special = true;
    multiplier = 135u;
  }
  uint32_t damage =
      roll_damage(game, b->enemy_attack, multiplier, 15u, game->hero.defense);
  if (b->guarding) damage = (damage * 2u + 4u) / 5u;
  if (damage == 0u) damage = 1u;
  b->hits[0] = damage;
  b->hit_count = 1;
  if (b->special)
    battle_append(game, text(H2_XIANXIA_RPG_TEXT_ENEMY_SPECIAL), info->name,
                  info->special, (unsigned)damage);
  else if (b->boss)
    battle_append(game, text(H2_XIANXIA_RPG_TEXT_BOSS_TAIL), (unsigned)damage);
  else
    battle_append(game, text(H2_XIANXIA_RPG_TEXT_ENEMY_ATTACK), info->name,
                  (unsigned)damage);
  if (b->guarding) battle_append(game, "%s", text(H2_XIANXIA_RPG_TEXT_GUARDED));
}

static void victory(h2_xianxia_rpg_game_t* game) {
  h2_xianxia_rpg_battle_t* b = &game->battle;
  h2_xianxia_rpg_hero_t* hero = &game->hero;
  const h2_xianxia_rpg_enemy_info_t* info =
      h2_xianxia_rpg_enemy_info((h2_xianxia_rpg_enemy_t)b->enemy);
  set_battle_state(game, H2_XIANXIA_RPG_BATTLE_VICTORY);
  ++game->battles_won;
  b->exp_gain = info->exp;
  hero->exp += info->exp;
  battle_say(game, text(H2_XIANXIA_RPG_TEXT_VICTORY), info->name,
             (unsigned)info->exp);
  if (!b->boss && hero->pills < H2_XIANXIA_RPG_PILLS_MAX &&
      random_below(game, 100u) < DROP_PERCENT) {
    ++hero->pills;
    battle_append(game, "%s", text(H2_XIANXIA_RPG_TEXT_DROP));
  }
  uint32_t level = hero->level;
  while (level < H2_XIANXIA_RPG_LEVEL_MAX &&
         hero->exp >= h2_xianxia_rpg_exp_for_level(level + 1u))
    ++level;
  if (level != hero->level) {
    const uint32_t exp = hero->exp, pills = hero->pills;
    h2_xianxia_rpg_hero_at_level(hero, level);
    hero->exp = exp;
    hero->pills = pills;
    battle_append(game, text(H2_XIANXIA_RPG_TEXT_LEVEL_UP), (unsigned)level);
  }
  if (b->boss) {
    advance_stage(game, H2_XIANXIA_RPG_STAGE_RETURN);
    /* A reload after the boss keeps it sealed. */
    request_save(game);
  }
}

static void hero_command(h2_xianxia_rpg_game_t* game,
                         h2_xianxia_rpg_command_t command) {
  h2_xianxia_rpg_battle_t* b = &game->battle;
  h2_xianxia_rpg_hero_t* hero = &game->hero;
  const h2_xianxia_rpg_enemy_info_t* info =
      h2_xianxia_rpg_enemy_info((h2_xianxia_rpg_enemy_t)b->enemy);
  if (!h2_xianxia_rpg_command_available(game, command)) {
    if (command == H2_XIANXIA_RPG_COMMAND_SKILL)
      battle_say(game, "%s",
                 text(game->stage < H2_XIANXIA_RPG_STAGE_TO_SHRINE
                          ? H2_XIANXIA_RPG_TEXT_NO_SKILL
                          : H2_XIANXIA_RPG_TEXT_NO_MP));
    else if (command == H2_XIANXIA_RPG_COMMAND_PILL)
      battle_say(game, "%s", text(H2_XIANXIA_RPG_TEXT_NO_PILLS));
    else if (command == H2_XIANXIA_RPG_COMMAND_FLEE)
      battle_say(game, "%s", text(H2_XIANXIA_RPG_TEXT_NO_FLEE));
    return;
  }
  b->command = (uint8_t)command;
  b->hit_count = b->hits_applied = 0;
  b->hit_on_enemy = true;
  b->special = false;
  ++game->turns_taken;
  set_battle_state(game, H2_XIANXIA_RPG_BATTLE_HERO_ACT);
  switch (command) {
    case H2_XIANXIA_RPG_COMMAND_ATTACK: {
      const bool critical = random_below(game, 100u) < CRIT_PERCENT;
      const uint32_t damage = roll_damage(
          game, hero->attack, critical ? 160u : 100u, 10u, b->enemy_defense);
      b->hits[0] = damage;
      b->hit_count = 1;
      b->special = critical;
      battle_say(game,
                 text(critical ? H2_XIANXIA_RPG_TEXT_CRITICAL
                               : H2_XIANXIA_RPG_TEXT_ATTACK),
                 info->name, (unsigned)damage);
      break;
    }
    case H2_XIANXIA_RPG_COMMAND_SKILL: {
      hero->mp -= H2_XIANXIA_RPG_SKILL_MP;
      uint32_t total = 0;
      for (uint32_t i = 0; i < H2_XIANXIA_RPG_MAX_HITS; ++i) {
        b->hits[i] = roll_damage(game, hero->attack, 75u, 8u, b->enemy_defense);
        total += b->hits[i];
      }
      b->hit_count = H2_XIANXIA_RPG_MAX_HITS;
      b->special = true;
      battle_say(game, text(H2_XIANXIA_RPG_TEXT_SKILL), (unsigned)total);
      break;
    }
    case H2_XIANXIA_RPG_COMMAND_PILL: {
      --hero->pills;
      const uint32_t heal_max = 40u + 8u * hero->level;
      const uint32_t room = hero->hp_max - hero->hp;
      const uint32_t healed = heal_max < room ? heal_max : room;
      hero->hp += healed;
      popup(game, false, true, healed, 0);
      battle_say(game, text(H2_XIANXIA_RPG_TEXT_PILL), (unsigned)healed);
      break;
    }
    case H2_XIANXIA_RPG_COMMAND_GUARD: {
      b->guarding = true;
      const uint32_t room = hero->mp_max - hero->mp;
      const uint32_t gained = GUARD_MP < room ? GUARD_MP : room;
      hero->mp += gained;
      battle_say(game, text(H2_XIANXIA_RPG_TEXT_GUARD), (unsigned)gained);
      break;
    }
    case H2_XIANXIA_RPG_COMMAND_FLEE:
      if (random_below(game, 100u) < FLEE_PERCENT) {
        set_battle_state(game, H2_XIANXIA_RPG_BATTLE_FLED);
        battle_say(game, "%s", text(H2_XIANXIA_RPG_TEXT_FLED));
      } else {
        battle_say(game, "%s", text(H2_XIANXIA_RPG_TEXT_FLEE_FAILED));
      }
      break;
  }
}

static uint32_t act_duration(const h2_xianxia_rpg_battle_t* b) {
  const uint32_t extra =
      b->hit_count > 1u ? (b->hit_count - 1u) * H2_XIANXIA_RPG_HIT_GAP_MS : 0u;
  return H2_XIANXIA_RPG_ACT_MS + extra;
}

static void apply_hits(h2_xianxia_rpg_game_t* game) {
  h2_xianxia_rpg_battle_t* b = &game->battle;
  while (b->hits_applied < b->hit_count &&
         b->state_ms >= H2_XIANXIA_RPG_HIT_MS +
                            b->hits_applied * H2_XIANXIA_RPG_HIT_GAP_MS) {
    const uint32_t damage = b->hits[b->hits_applied];
    if (b->hit_on_enemy) {
      b->enemy_hp = damage < b->enemy_hp ? b->enemy_hp - damage : 0u;
      b->enemy_flash_ms = H2_XIANXIA_RPG_FLASH_MS;
    } else {
      game->hero.hp = damage < game->hero.hp ? game->hero.hp - damage : 0u;
      b->hero_flash_ms = H2_XIANXIA_RPG_FLASH_MS;
    }
    popup(game, b->hit_on_enemy, false, damage, (int32_t)b->hits_applied);
    ++b->hits_applied;
  }
}

static void battle_step(h2_xianxia_rpg_game_t* game, uint32_t dt_ms) {
  h2_xianxia_rpg_battle_t* b = &game->battle;
  b->state_ms += dt_ms;
  b->enemy_flash_ms = b->enemy_flash_ms > dt_ms ? b->enemy_flash_ms - dt_ms : 0;
  b->hero_flash_ms = b->hero_flash_ms > dt_ms ? b->hero_flash_ms - dt_ms : 0;
  for (unsigned i = 0; i < H2_XIANXIA_RPG_MAX_POPUPS; ++i) {
    h2_xianxia_rpg_popup_t* p = &b->popups[i];
    if (!p->active) continue;
    p->age_ms += dt_ms;
    if (p->age_ms >= H2_XIANXIA_RPG_POPUP_MS) p->active = false;
  }
  switch (b->state) {
    case H2_XIANXIA_RPG_BATTLE_INTRO:
      if (b->state_ms >= H2_XIANXIA_RPG_INTRO_MS) {
        set_battle_state(game, H2_XIANXIA_RPG_BATTLE_COMMAND);
        battle_say(game, "%s", text(H2_XIANXIA_RPG_TEXT_CHOOSE));
      }
      break;
    case H2_XIANXIA_RPG_BATTLE_HERO_ACT:
      apply_hits(game);
      if (b->state_ms >= act_duration(b)) {
        if (b->enemy_hp == 0u)
          victory(game);
        else
          enemy_turn(game);
      }
      break;
    case H2_XIANXIA_RPG_BATTLE_ENEMY_ACT:
      apply_hits(game);
      if (b->state_ms >= act_duration(b)) {
        b->guarding = false;
        ++b->turn;
        if (game->hero.hp == 0u) {
          set_battle_state(game, H2_XIANXIA_RPG_BATTLE_DEFEAT);
          battle_say(game, "%s", text(H2_XIANXIA_RPG_TEXT_DEFEAT));
        } else {
          set_battle_state(game, H2_XIANXIA_RPG_BATTLE_COMMAND);
        }
      }
      break;
    case H2_XIANXIA_RPG_BATTLE_FLED:
      if (b->state_ms >= FLED_RETURN_MS) {
        enter_phase(game, H2_XIANXIA_RPG_PHASE_EXPLORE);
        reset_encounter(game);
      }
      break;
    default:
      break;
  }
}

static void battle_finish(h2_xianxia_rpg_game_t* game) {
  h2_xianxia_rpg_battle_t* b = &game->battle;
  if (b->state == H2_XIANXIA_RPG_BATTLE_VICTORY) {
    enter_phase(game, H2_XIANXIA_RPG_PHASE_EXPLORE);
    reset_encounter(game);
    if (b->boss) start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_VICTORY);
  } else if (b->state == H2_XIANXIA_RPG_BATTLE_DEFEAT) {
    restore(&game->hero);
    place_hero(game, game->rest_map, game->rest_x, game->rest_y);
    game->facing = H2_XIANXIA_RPG_FACING_DOWN;
    enter_phase(game, H2_XIANXIA_RPG_PHASE_EXPLORE);
    start_dialogue(game, b->boss ? H2_XIANXIA_RPG_SCRIPT_REVIVE_BOSS
                                 : H2_XIANXIA_RPG_SCRIPT_REVIVE);
  } else if (b->state == H2_XIANXIA_RPG_BATTLE_FLED) {
    enter_phase(game, H2_XIANXIA_RPG_PHASE_EXPLORE);
    reset_encounter(game);
  }
}

static void battle_tap(h2_xianxia_rpg_game_t* game, float x, float y) {
  h2_xianxia_rpg_battle_t* b = &game->battle;
  switch (b->state) {
    case H2_XIANXIA_RPG_BATTLE_INTRO:
      if (b->state_ms >= H2_XIANXIA_RPG_TAP_GUARD_MS) {
        set_battle_state(game, H2_XIANXIA_RPG_BATTLE_COMMAND);
        battle_say(game, "%s", text(H2_XIANXIA_RPG_TEXT_CHOOSE));
      }
      break;
    case H2_XIANXIA_RPG_BATTLE_COMMAND:
      for (int32_t i = 0; i < H2_XIANXIA_RPG_COMMANDS; ++i)
        if (inside(&game->layout.commands[i], x, y)) {
          hero_command(game, (h2_xianxia_rpg_command_t)i);
          break;
        }
      break;
    case H2_XIANXIA_RPG_BATTLE_VICTORY:
    case H2_XIANXIA_RPG_BATTLE_DEFEAT:
    case H2_XIANXIA_RPG_BATTLE_FLED:
      if (b->state_ms >= VICTORY_TAP_MS) battle_finish(game);
      break;
    default:
      break;
  }
}

/* ---- Exploration ------------------------------------------------------- */

static void apply_effect(h2_xianxia_rpg_game_t* game,
                         h2_xianxia_rpg_effect_t effect) {
  h2_xianxia_rpg_hero_t* hero = &game->hero;
  switch (effect) {
    case H2_XIANXIA_RPG_EFFECT_BRIEFED:
      if (game->stage == H2_XIANXIA_RPG_STAGE_MEET_ELDER)
        advance_stage(game, H2_XIANXIA_RPG_STAGE_SEEK_DAOIST);
      request_save(game);
      break;
    case H2_XIANXIA_RPG_EFFECT_GIVE_PILLS:
      game->flags |= H2_XIANXIA_RPG_FLAG_PILLS_GIVEN;
      if (hero->pills < 3u) hero->pills = 3u;
      break;
    case H2_XIANXIA_RPG_EFFECT_TAUGHT:
      if (game->stage == H2_XIANXIA_RPG_STAGE_SEEK_DAOIST) {
        advance_stage(game, H2_XIANXIA_RPG_STAGE_TO_SHRINE);
        restore(hero);
        hero->pills = hero->pills + 2u < H2_XIANXIA_RPG_PILLS_MAX
                          ? hero->pills + 2u
                          : H2_XIANXIA_RPG_PILLS_MAX;
        request_save(game);
      }
      break;
    case H2_XIANXIA_RPG_EFFECT_REST:
      restore(hero);
      game->rest_map = game->map;
      game->rest_x = game->tx;
      game->rest_y = game->ty;
      request_save(game);
      break;
    case H2_XIANXIA_RPG_EFFECT_BOSS_BATTLE:
      start_battle(game, H2_XIANXIA_RPG_ENEMY_DRAGON);
      break;
    case H2_XIANXIA_RPG_EFFECT_ENDING:
      game->stage = H2_XIANXIA_RPG_STAGE_CLEARED;
      /* A reload after the ending reopens the result. */
      request_save(game);
      enter_phase(game, H2_XIANXIA_RPG_PHASE_ENDING);
      break;
    default:
      break;
  }
}

static void advance_dialogue(h2_xianxia_rpg_game_t* game) {
  h2_xianxia_rpg_dialogue_t* d = &game->dialogue;
  if (!d->active) return;
  if (d->revealed < d->total) {
    /* Keep the typewriter clock in step, or the next frame rewinds it. */
    d->revealed = d->total;
    d->reveal_ms = d->total * H2_XIANXIA_RPG_REVEAL_MS_PER_CHAR;
    return;
  }
  if ((uint32_t)d->index + 1u < d->script->count) {
    ++d->index;
    d->revealed = 0;
    d->reveal_ms = 0;
    d->total = h2_xianxia_rpg_utf8_length(d->script->lines[d->index].text);
    return;
  }
  const h2_xianxia_rpg_effect_t effect =
      (h2_xianxia_rpg_effect_t)d->script->effect;
  d->active = false;
  d->script = NULL;
  apply_effect(game, effect);
}

static h2_xianxia_rpg_facing_t facing_toward(int32_t dx, int32_t dy) {
  if (dx > 0) return H2_XIANXIA_RPG_FACING_RIGHT;
  if (dx < 0) return H2_XIANXIA_RPG_FACING_LEFT;
  return dy < 0 ? H2_XIANXIA_RPG_FACING_UP : H2_XIANXIA_RPG_FACING_DOWN;
}

static void interact(h2_xianxia_rpg_game_t* game, int32_t x, int32_t y) {
  game->facing = facing_toward(x - game->tx, y - game->ty);
  h2_xianxia_rpg_npc_t npc;
  if (npc_at(game, x, y, &npc)) {
    start_dialogue(game, h2_xianxia_rpg_talk_script(
                             game, (h2_xianxia_rpg_actor_t)npc.actor));
    return;
  }
  switch (h2_xianxia_rpg_tile(game->map, x, y)) {
    case 'W':
      start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_WELL);
      break;
    case 'S':
      start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_SPRING);
      break;
    case 'X':
      if (game->stage < H2_XIANXIA_RPG_STAGE_TO_SHRINE)
        start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_BARRIER);
      break;
    default:
      break;
  }
}

static void arrive(h2_xianxia_rpg_game_t* game) {
  h2_xianxia_rpg_map_t to_map;
  int32_t to_x, to_y;
  if (h2_xianxia_rpg_exit_at(game->map, game->tx, game->ty, &to_map, &to_x,
                             &to_y)) {
    place_hero(game, to_map, to_x, to_y);
    particles_reset(game);
    return;
  }
  h2_xianxia_rpg_enemy_t enemies[2];
  const size_t count = h2_xianxia_rpg_encounters(game->map, enemies);
  if (count == 0u || game->stage >= H2_XIANXIA_RPG_STAGE_RETURN) return;
  if (game->encounter_steps > 1u) {
    --game->encounter_steps;
    return;
  }
  reset_encounter(game);
  start_battle(game, enemies[random_below(game, (uint32_t)count)]);
}

static void explore_step(h2_xianxia_rpg_game_t* game, uint32_t dt_ms) {
  game->banner_ms = game->banner_ms > dt_ms ? game->banner_ms - dt_ms : 0u;
  if (!game->dialogue.active && game->banner_ms == 0u)
    game->objective_ms =
        game->objective_ms > dt_ms ? game->objective_ms - dt_ms : 0u;
  game->marker_ms = game->marker_ms > dt_ms ? game->marker_ms - dt_ms : 0u;
  h2_xianxia_rpg_dialogue_t* d = &game->dialogue;
  if (d->active) {
    d->reveal_ms += dt_ms;
    const uint32_t shown = d->reveal_ms / H2_XIANXIA_RPG_REVEAL_MS_PER_CHAR;
    d->revealed = shown < d->total ? shown : d->total;
    return;
  }
  if (game->moving) {
    game->move_ms += dt_ms;
    if (game->move_ms < H2_XIANXIA_RPG_STEP_MS) return;
    game->moving = false;
    game->move_ms = 0;
    game->from_x = game->tx;
    game->from_y = game->ty;
    arrive(game);
    if (game->phase != H2_XIANXIA_RPG_PHASE_EXPLORE) return;
  }
  if (game->path_pos < game->path_len) {
    const int32_t nx = game->path_x[game->path_pos];
    const int32_t ny = game->path_y[game->path_pos];
    ++game->path_pos;
    if (!passable(game, nx, ny)) {
      clear_path(game);
      return;
    }
    game->facing = facing_toward(nx - game->tx, ny - game->ty);
    game->from_x = game->tx;
    game->from_y = game->ty;
    game->tx = nx;
    game->ty = ny;
    game->moving = true;
    game->move_ms = 0;
    ++game->walk_steps;
    return;
  }
  if (game->target_x >= 0) {
    const int32_t x = game->target_x, y = game->target_y;
    game->target_x = game->target_y = -1;
    interact(game, x, y);
  }
}

static void explore_tap(h2_xianxia_rpg_game_t* game, float x, float y) {
  if (game->dialogue.active) {
    advance_dialogue(game);
    return;
  }
  const h2_xianxia_rpg_layout_t* l = &game->layout;
  const float fx = (x - (float)l->map_x) / (float)l->tile;
  const float fy = (y - (float)l->map_y) / (float)l->tile;
  if (!(fx >= 0.0f && fy >= 0.0f)) return;
  const int32_t col = (int32_t)fx, row = (int32_t)fy;
  if (col >= H2_XIANXIA_RPG_MAP_COLS || row >= H2_XIANXIA_RPG_MAP_ROWS) return;
  /* Paths start at the tile the hero is entering. */
  if (interactive(game, col, row)) {
    const int32_t distance =
        (col > game->tx ? col - game->tx : game->tx - col) +
        (row > game->ty ? row - game->ty : game->ty - row);
    if (distance == 1 && !game->moving) {
      clear_path(game);
      interact(game, col, row);
      return;
    }
    if (find_path(game, col, row, true)) {
      game->target_x = col;
      game->target_y = row;
      game->marker_ms = 0;
    }
    return;
  }
  if (!passable(game, col, row)) return;
  if (find_path(game, col, row, false)) {
    game->target_x = game->target_y = -1;
    game->marker_x = col;
    game->marker_y = row;
    game->marker_ms = MARKER_MS;
  }
}

/* ---- Title and ending -------------------------------------------------- */

static void new_game(h2_xianxia_rpg_game_t* game) {
  h2_xianxia_rpg_hero_at_level(&game->hero, 1u);
  game->stage = H2_XIANXIA_RPG_STAGE_MEET_ELDER;
  game->objective_ms = 0;
  game->flags = 0;
  game->battles_won = game->turns_taken = 0;
  game->facing = H2_XIANXIA_RPG_FACING_UP;
  place_hero(game, H2_XIANXIA_RPG_MAP_VILLAGE, 7, 5);
  game->rest_map = H2_XIANXIA_RPG_MAP_VILLAGE;
  game->rest_x = 8;
  game->rest_y = 5;
  enter_phase(game, H2_XIANXIA_RPG_PHASE_EXPLORE);
  start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_INTRO);
}

static void continue_game(h2_xianxia_rpg_game_t* game) {
  const h2_xianxia_rpg_save_t* s = &game->saved;
  h2_xianxia_rpg_hero_at_level(&game->hero, s->level);
  if (s->exp > game->hero.exp) game->hero.exp = s->exp;
  game->hero.pills = s->pills;
  game->stage = (h2_xianxia_rpg_stage_t)s->stage;
  game->flags = s->flags;
  game->battles_won = s->battles;
  game->turns_taken = s->turns;
  game->facing = H2_XIANXIA_RPG_FACING_DOWN;
  h2_xianxia_rpg_map_t map = (h2_xianxia_rpg_map_t)s->map;
  int32_t x = (int32_t)s->x, y = (int32_t)s->y;
  game->map = map;
  if (!h2_xianxia_rpg_walkable(game, map, x, y) || npc_at(game, x, y, NULL)) {
    map = H2_XIANXIA_RPG_MAP_VILLAGE;
    x = 8;
    y = 5;
  }
  place_hero(game, map, x, y);
  game->rest_map = map;
  game->rest_x = x;
  game->rest_y = y;
  /* A finished chapter reopens its result rather than the boss fight. */
  enter_phase(game, game->stage == H2_XIANXIA_RPG_STAGE_CLEARED
                        ? H2_XIANXIA_RPG_PHASE_ENDING
                        : H2_XIANXIA_RPG_PHASE_EXPLORE);
}

static void title_tap(h2_xianxia_rpg_game_t* game, float x, float y) {
  if (inside(&game->layout.title_buttons[0], x, y))
    new_game(game);
  else if (game->has_save && inside(&game->layout.title_buttons[1], x, y))
    continue_game(game);
}

/* ---- Public entry points ---------------------------------------------- */

void h2_xianxia_rpg_game_init(h2_xianxia_rpg_game_t* game, int32_t width,
                              int32_t height, uint32_t seed,
                              const h2_xianxia_rpg_save_t* save) {
  memset(game, 0, sizeof(*game));
  game->width = width;
  game->height = height;
  const int32_t short_side = width < height ? width : height;
  game->unit = (float)short_side / 480.0f;
  game->rng = seed != 0u ? seed : 0x5EEDu;
  game->target_x = game->target_y = -1;
  compute_layout(game);
  if (save != NULL) {
    uint32_t words[H2_XIANXIA_RPG_SAVE_WORDS];
    h2_xianxia_rpg_save_pack(save, words);
    game->has_save = h2_xianxia_rpg_save_unpack(words, &game->saved);
  }
  h2_xianxia_rpg_hero_at_level(&game->hero, 1u);
  enter_phase(game, H2_XIANXIA_RPG_PHASE_TITLE);
}

void h2_xianxia_rpg_game_pointer(h2_xianxia_rpg_game_t* game,
                                 h2_xianxia_rpg_pointer_t kind, float x,
                                 float y) {
  if (kind != H2_XIANXIA_RPG_POINTER_DOWN) return;
  if (game->time_ms < game->phase_started_ms + H2_XIANXIA_RPG_TAP_GUARD_MS)
    return;
  switch (game->phase) {
    case H2_XIANXIA_RPG_PHASE_TITLE:
      title_tap(game, x, y);
      break;
    case H2_XIANXIA_RPG_PHASE_EXPLORE:
      explore_tap(game, x, y);
      break;
    case H2_XIANXIA_RPG_PHASE_BATTLE:
      battle_tap(game, x, y);
      break;
    case H2_XIANXIA_RPG_PHASE_ENDING:
      if (game->time_ms >= game->phase_started_ms + ENDING_TAP_MS)
        enter_phase(game, H2_XIANXIA_RPG_PHASE_TITLE);
      break;
  }
}

void h2_xianxia_rpg_game_step(h2_xianxia_rpg_game_t* game, uint32_t dt_ms) {
  while (dt_ms > 0u) {
    const uint32_t slice = dt_ms > STEP_SLICE_MS ? STEP_SLICE_MS : dt_ms;
    dt_ms -= slice;
    game->time_ms += slice;
    if (game->phase == H2_XIANXIA_RPG_PHASE_EXPLORE)
      explore_step(game, slice);
    else if (game->phase == H2_XIANXIA_RPG_PHASE_BATTLE)
      battle_step(game, slice);
    particles_step(game, slice);
  }
}

bool h2_xianxia_rpg_game_take_save(h2_xianxia_rpg_game_t* game,
                                   h2_xianxia_rpg_save_t* out_save) {
  if (!game->save_pending) return false;
  game->save_pending = false;
  const h2_xianxia_rpg_save_t save = {
      .stage = (uint32_t)game->stage,
      .level = game->hero.level,
      .exp = game->hero.exp,
      .pills = game->hero.pills,
      .flags = game->flags,
      .map = (uint32_t)game->map,
      .x = (uint32_t)game->tx,
      .y = (uint32_t)game->ty,
      .battles = game->battles_won,
      .turns = game->turns_taken,
  };
  game->saved = save;
  game->has_save = true;
  if (out_save != NULL) *out_save = save;
  return true;
}

void h2_xianxia_rpg_save_pack(const h2_xianxia_rpg_save_t* save,
                              uint32_t words[H2_XIANXIA_RPG_SAVE_WORDS]) {
  words[0] = H2_XIANXIA_RPG_SAVE_VERSION | (save->stage & 0xFFu) << 8 |
             (save->level & 0xFFu) << 16 | (save->pills & 0xFFu) << 24;
  words[1] = save->exp;
  words[2] = (save->map & 0xFFu) | (save->x & 0xFFu) << 8 |
             (save->y & 0xFFu) << 16 | (save->flags & 0xFFu) << 24;
  const uint32_t battles = save->battles < 0xFFFFu ? save->battles : 0xFFFFu;
  const uint32_t turns = save->turns < 0xFFFFu ? save->turns : 0xFFFFu;
  words[3] = battles | turns << 16;
}

bool h2_xianxia_rpg_save_unpack(const uint32_t words[H2_XIANXIA_RPG_SAVE_WORDS],
                                h2_xianxia_rpg_save_t* out_save) {
  if ((words[0] & 0xFFu) != H2_XIANXIA_RPG_SAVE_VERSION) return false;
  const h2_xianxia_rpg_save_t save = {
      .stage = words[0] >> 8 & 0xFFu,
      .level = words[0] >> 16 & 0xFFu,
      .pills = words[0] >> 24 & 0xFFu,
      .exp = words[1],
      .map = words[2] & 0xFFu,
      .x = words[2] >> 8 & 0xFFu,
      .y = words[2] >> 16 & 0xFFu,
      .flags = words[2] >> 24 & 0xFFu,
      .battles = words[3] & 0xFFFFu,
      .turns = words[3] >> 16,
  };
  if (save.stage > (uint32_t)H2_XIANXIA_RPG_STAGE_CLEARED || save.level < 1u ||
      save.level > H2_XIANXIA_RPG_LEVEL_MAX ||
      save.pills > H2_XIANXIA_RPG_PILLS_MAX ||
      save.map >= (uint32_t)H2_XIANXIA_RPG_MAP_COUNT ||
      save.x >= (uint32_t)H2_XIANXIA_RPG_MAP_COLS ||
      save.y >= (uint32_t)H2_XIANXIA_RPG_MAP_ROWS ||
      (save.flags & ~H2_XIANXIA_RPG_FLAG_PILLS_GIVEN) != 0u)
    return false;
  *out_save = save;
  return true;
}
