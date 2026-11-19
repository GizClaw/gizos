#include "h2_xianxia_rpg_game.h"

#include "h2_xianxia_rpg_story.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STEP_SLICE_MS 50u
#define MARKER_MS 600u
#define VICTORY_TAP_MS 450u
#define ENDING_TAP_MS 1500u
#define PROLOGUE_TAP_MS 900u
#define FLED_RETURN_MS 1400u
#define ENCOUNTER_MIN_STEPS 9u
#define ENCOUNTER_SPAN_STEPS 9u
#define DROP_PERCENT 35u
#define CRIT_PERCENT 12u
#define FLEE_PERCENT 65u
#define GUARD_MP 4u
#define LINE_PX H2_XIANXIA_RPG_LINE_PX

/* Companion battles. A veiled enemy takes 30 % damage; the pole strike is
 * 40 % of the hero's attack; the joint strike is about 1.8 times the
 * three-strike sword art and breaks a charge. */
#define VEIL_PERCENT 30u
#define HINDER_PERCENT 70u
#define POLE_PERCENT 40u
#define JOINT_PERCENT_A 200u
#define JOINT_PERCENT_B 205u
#define STEADY_PERCENT 75u
#define GHOST_VEIL_PERCENT 25u
#define TUTORIAL_GHOST_HP 80u
/** Hitting the wrong shadow lets the wraith regain this share of its HP. */
#define WRONG_SHADOW_HEAL_PERCENT 5u

#define RECORD_SEED_JOURNEY 0x4A524E59u
#define RECORD_SEED_PROGRESS 0x50524F47u
#define RECORD_SEED_CHAPTER1 0x56315341u

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

  /* Title buttons keep their v0.1 places; the chapter button follows. */
  const int32_t button_w = w * 6 / 10 < 280 ? w * 6 / 10 : 280;
  const int32_t button_h = h < 300 ? 34 : h < 360 ? 40 : 48;
  const int32_t button_gap = h < 300 ? 6 : h < 360 ? 8 : 14;
  int32_t button_y = h * 60 / 100;
  const int32_t buttons_end = button_y +
                              H2_XIANXIA_RPG_TITLE_BUTTONS * button_h +
                              (H2_XIANXIA_RPG_TITLE_BUTTONS - 1) * button_gap;
  if (buttons_end > h - 8) button_y -= buttons_end - (h - 8);
  for (int32_t i = 0; i < H2_XIANXIA_RPG_TITLE_BUTTONS; ++i)
    l->title_buttons[i] =
        rect((w - button_w) / 2, button_y + i * (button_h + button_gap),
             button_w, button_h);

  /* Chapter screen: a heading, one row per chapter and a way back. */
  const int32_t row_gap = h < 400 ? 1 : 4;
  const int32_t visible_rows = h < 400 ? 5 : H2_XIANXIA_RPG_CHAPTER_ROWS;
  const int32_t head_h = h < 300 ? LINE_PX : LINE_PX + 8;
  const int32_t row_h =
      (h - head_h - 32 - visible_rows * row_gap) / (visible_rows + 1);
  const int32_t back_h = row_h;
  const int32_t panel_w = w - 16 < 560 ? w - 16 : 560;
  const int32_t panel_h =
      10 + head_h + visible_rows * (row_h + row_gap) + back_h + 10;
  const int32_t panel_y = (h - panel_h) / 2 > 4 ? (h - panel_h) / 2 : 4;
  l->chapter_panel = rect((w - panel_w) / 2, panel_y, panel_w, panel_h);
  const int32_t restart_w = w < 400 ? 56 : 72;
  for (int32_t i = 0; i < H2_XIANXIA_RPG_CHAPTER_ROWS; ++i) {
    const h2_xianxia_rpg_rect_t row =
        rect(l->chapter_panel.x + 10,
             panel_y + 10 + head_h + (i % visible_rows) * (row_h + row_gap),
             panel_w - 20, row_h);
    l->chapter_rows[i] = row;
    l->chapter_restart[i] =
        rect(row.x + row.w - restart_w - 6, row.y + 6, restart_w, row.h - 12);
  }
  const int32_t back_w = panel_w * 4 / 10 < 180 ? panel_w * 4 / 10 : 180;
  l->chapter_back = rect(
      (w - back_w) / 2,
      panel_y + 10 + head_h + visible_rows * (row_h + row_gap), back_w, back_h);
  if (h < 400) {
    l->chapter_back.x = l->chapter_panel.x + 10;
    l->chapter_back.w = (panel_w - 30) / 2;
    l->chapter_more = l->chapter_back;
    l->chapter_more.x += l->chapter_back.w + 10;
  }

  const int32_t message_h = 2 * LINE_PX + 2 * pad;
  const int32_t command_h = 46;
  const int32_t battle_panel_h = message_h + command_h + 3 * pad;
  l->stage = rect(0, 0, w, h - battle_panel_h);
  l->panel = rect(0, h - battle_panel_h, w, battle_panel_h);
  l->message = rect(pad, l->panel.y + pad, w - 2 * pad, message_h);
  const int32_t command_w =
      (w - 2 * pad - (H2_XIANXIA_RPG_COMMANDS - 1) * pad) /
      H2_XIANXIA_RPG_COMMANDS;
  for (int32_t i = 0; i < H2_XIANXIA_RPG_COMMANDS; ++i)
    l->commands[i] = rect(pad + i * (command_w + pad),
                          l->message.y + message_h + pad, command_w, command_h);

  const int32_t action_w = (w - 24) / 2;
  l->escort_actions[0] = rect(8, h - 48, action_w, 40);
  l->escort_actions[1] = rect(16 + action_w, h - 48, action_w, 40);
  const int32_t party_y = h < 300 ? 0 : 6;
  const int32_t party_h = h < 300 ? 44 : 48;
  l->party[0] = rect(8, party_y, action_w, party_h);
  l->party[1] = rect(16 + action_w, party_y, action_w, party_h);

  l->glyph_panel = rect(8, 8, w - 16, h - 16);
  const int32_t glyph_w = (w - 48) / 3;
  const int32_t glyph_h = h < 300 ? 42 : 72;
  for (int32_t i = 0; i < 3; ++i)
    l->glyph_choices[i] = rect(16 + i * (glyph_w + 8), h / 2, glyph_w, glyph_h);
  l->glyph_back = rect(w / 2 - 60, h - 48, 120, 36);

  /* The renderer draws the shadows of a split boss inside these boxes. */
  const float sh = (float)l->stage.h;
  const float size =
      sh * 0.5f < (float)w * 0.26f ? sh * 0.5f : (float)w * 0.26f;
  const float shadow_y = sh * 0.6f;
  static const float k_shadow_x[2] = {0.52f, 0.84f};
  for (int32_t i = 0; i < 2; ++i) {
    const float cx = (float)w * k_shadow_x[i];
    l->shadows[i] =
        rect((int32_t)(cx - size * 0.45f), (int32_t)(shadow_y - size * 0.75f),
             (int32_t)(size * 0.9f), (int32_t)(size * 1.3f));
  }
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
  const char tile = h2_xianxia_rpg_tile(map, x, y);
  if (h2_xianxia_rpg_map_chapter(map) == 9u) return tile == '.' || tile == '=';
  if (h2_xianxia_rpg_map_chapter(map) == 8u)
    return h2_xianxia_rpg_c8_walkable(game, map, tile);
  if (h2_xianxia_rpg_map_chapter(map) == 7u)
    return h2_xianxia_rpg_c7_walkable(game, tile);
  if (h2_xianxia_rpg_map_chapter(map) == 6u)
    return tile == '.' || tile == '=' || tile == 'i' ||
           (tile == 'h' && map == H2_XIANXIA_RPG_MAP_HOT_SPRING);
  if (h2_xianxia_rpg_map_chapter(map) == 5u)
    return h2_xianxia_rpg_c5_walkable(game, tile);
  if (h2_xianxia_rpg_map_chapter(map) == 4u)
    return tile == '.' || tile == ',' || tile == '=' || tile == 'b' ||
           tile == '_' || tile == 'w';
  if (h2_xianxia_rpg_map_chapter(map) == 3u)
    return h2_xianxia_rpg_c3_walkable(game, tile);
  if (h2_xianxia_rpg_map_chapter(map) != 1u)
    return h2_xianxia_rpg_c2_walkable(game, tile);
  switch (tile) {
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
  return npc_at(game, x, y, NULL) ||
         h2_xianxia_rpg_tile_interactive(game, x, y);
}

/* Breadth-first path from the hero tile to the goal, or to any passable
 * neighbor of it when adjacent is true. With avoid set, ambush ground is
 * entered only as the goal itself. Replaces the current path. */
static bool search_path(h2_xianxia_rpg_game_t* game, int32_t goal_x,
                        int32_t goal_y, bool adjacent, bool avoid) {
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
      if (avoid && h2_xianxia_rpg_costly_cell(game->map, nx, ny) &&
          !(!adjacent && nx == goal_x && ny == goal_y))
        continue;
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

/* Prefer a way around tall reeds; cross them only when nothing else
 * reaches the goal. */
static bool find_path(h2_xianxia_rpg_game_t* game, int32_t goal_x,
                      int32_t goal_y, bool adjacent) {
  return search_path(game, goal_x, goal_y, adjacent, true) ||
         search_path(game, goal_x, goal_y, adjacent, false);
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
    case 5: /* snow */
      p->vx = -12.0f * u;
      p->vy = 22.0f * u;
      break;
    case 4: /* slanted rain */
      p->vx = -32.0f * u;
      p->vy = 220.0f * u;
      p->life_ms = 3000u;
      break;
    case 3: /* river mist drifting with the current */
      if (!anywhere) {
        p->x = (float)area->x - 20.0f * u;
        p->y = (float)area->y + random_unit(game) * (float)area->h;
      }
      p->vx = (5.0f + 6.0f * random_unit(game)) * u;
      p->vy = (random_unit(game) - 0.5f) * 3.0f * u;
      p->life_ms = 16000u + random_below(game, 8000u);
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
    const bool foggy = game->stage < H2_XIANXIA_RPG_STAGE_C2_RETURN;
    switch (game->map) {
      case H2_XIANXIA_RPG_MAP_SHAFT_UPPER:
      case H2_XIANXIA_RPG_MAP_SHAFT_LOWER:
      case H2_XIANXIA_RPG_MAP_ECHO_HALL:
      case H2_XIANXIA_RPG_MAP_LANTERN_CORE:
        kind = 2;
        count = 5;
        break;
      case H2_XIANXIA_RPG_MAP_SNOW_CAMP:
      case H2_XIANXIA_RPG_MAP_ICE_CLIFF:
      case H2_XIANXIA_RPG_MAP_SNOW_CAVE:
        kind = 5;
        count = game->map == H2_XIANXIA_RPG_MAP_SNOW_CAVE ? 3u : 12u;
        break;
      case H2_XIANXIA_RPG_MAP_RAIN_PATH:
      case H2_XIANXIA_RPG_MAP_SWORD_COURT:
      case H2_XIANXIA_RPG_MAP_SWORD_HALL:
        kind = 4;
        count = game->stage < H2_XIANXIA_RPG_STAGE_C3_RETURN ? 18u : 5u;
        break;
      case H2_XIANXIA_RPG_MAP_FOREST:
        kind = 0;
        count = 6;
        break;
      case H2_XIANXIA_RPG_MAP_SHRINE:
        kind = 2;
        count = 7;
        break;
      case H2_XIANXIA_RPG_MAP_DOCK:
        kind = 3;
        count = foggy ? 6u : 0u;
        break;
      case H2_XIANXIA_RPG_MAP_REEDS:
        kind = 3;
        count = 8;
        break;
      case H2_XIANXIA_RPG_MAP_WRECK:
        kind = foggy ? 3 : 2;
        count = 6;
        break;
      default:
        kind = 1;
        count = 4;
        break;
    }
  } else if (game->phase == H2_XIANXIA_RPG_PHASE_TITLE ||
             game->phase == H2_XIANXIA_RPG_PHASE_ENDING ||
             game->phase == H2_XIANXIA_RPG_PHASE_PROLOGUE) {
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
    const float margin = p->kind == 3 ? 30.0f * game->unit : 8.0f;
    const bool out = p->x < (float)area.x - margin ||
                     p->x > (float)(area.x + area.w) + margin ||
                     p->y < (float)area.y - margin ||
                     p->y > (float)(area.y + area.h) + margin;
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

bool h2_xianxia_rpg_ally_joined(const h2_xianxia_rpg_game_t* game) {
  if (game->chapter == 6u)
    return (game->facts & H2_XIANXIA_RPG_FACT_C6_HEALED) != 0u;
  return game->chapter >= 3u ||
         (game->chapter == 2u &&
          game->stage >= H2_XIANXIA_RPG_STAGE_C2_TO_REEDS);
}

/* The companion appears beside the hero, behind them when possible. */
static void place_ally(h2_xianxia_rpg_game_t* game) {
  game->ally_visible = game->chapter == 6u || h2_xianxia_rpg_ally_joined(game);
  int32_t x = game->tx, y = game->ty;
  if (game->ally_visible) {
    int32_t bx = 0, by = 0;
    switch (game->facing) {
      case H2_XIANXIA_RPG_FACING_UP:
        by = 1;
        break;
      case H2_XIANXIA_RPG_FACING_LEFT:
        bx = 1;
        break;
      case H2_XIANXIA_RPG_FACING_RIGHT:
        bx = -1;
        break;
      default:
        by = -1;
        break;
    }
    const int32_t options[4][2] = {{bx, by}, {by, bx}, {-by, -bx}, {-bx, -by}};
    for (int32_t i = 0; i < 4; ++i) {
      const int32_t cx = game->tx + options[i][0];
      const int32_t cy = game->ty + options[i][1];
      if (cx < 0 || cy < 0 || cx >= H2_XIANXIA_RPG_MAP_COLS ||
          cy >= H2_XIANXIA_RPG_MAP_ROWS || !passable(game, cx, cy))
        continue;
      h2_xianxia_rpg_map_t to;
      int32_t ex, ey;
      if (h2_xianxia_rpg_exit_at(game->map, cx, cy, &to, &ex, &ey)) continue;
      x = cx;
      y = cy;
      break;
    }
  }
  game->ally_x = game->ally_from_x = x;
  game->ally_y = game->ally_from_y = y;
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
  place_ally(game);
}

static uint32_t dialogue_page_length(const h2_xianxia_rpg_game_t* game) {
  const h2_xianxia_rpg_dialogue_t* d = &game->dialogue;
  const h2_xianxia_rpg_line_t* line = &d->script->lines[d->index];
  const uint32_t remaining =
      h2_xianxia_rpg_utf8_length(line->text + d->page_offset);
  if (game->chapter < 5u) return remaining;
  /* The body font is 18 px; using that full advance for every codepoint is
   * conservative for ASCII. Keep every chapter-5 instruction readable on
   * the supported 320-pixel display instead of clipping its last lines. */
  const uint32_t cols = (uint32_t)(game->layout.dialog.w - 28) / 18u;
  const uint32_t rows =
      (uint32_t)game->layout.dialog_lines +
      (line->speaker == H2_XIANXIA_RPG_ACTOR_NARRATOR ? 1u : 0u);
  const uint32_t capacity = cols * rows;
  return remaining < capacity ? remaining : capacity;
}

static void start_dialogue(h2_xianxia_rpg_game_t* game,
                           h2_xianxia_rpg_script_id_t id) {
  const h2_xianxia_rpg_script_t* script = h2_xianxia_rpg_script(id);
  if (script == NULL || script->count == 0u) return;
  h2_xianxia_rpg_dialogue_t* d = &game->dialogue;
  memset(d, 0, sizeof(*d));
  d->active = true;
  d->script = script;
  d->total = dialogue_page_length(game);
}

static void request_save(h2_xianxia_rpg_game_t* game) {
  game->save_pending = true;
}

static void advance_stage(h2_xianxia_rpg_game_t* game,
                          h2_xianxia_rpg_stage_t stage) {
  game->stage = stage;
  game->objective_ms = H2_XIANXIA_RPG_OBJECTIVE_MS;
}

static void restore_party(h2_xianxia_rpg_game_t* game) {
  restore(&game->hero);
  game->ally_hp_max = 60u + 6u * game->hero.level;
  game->ally_hp = game->ally_hp_max;
}

/* Full strength, and the hero's current cell becomes the rest point. */
static void rest_here(h2_xianxia_rpg_game_t* game) {
  restore_party(game);
  game->rest_map = game->map;
  game->rest_x = game->tx;
  game->rest_y = game->ty;
  request_save(game);
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

static bool is_boss(h2_xianxia_rpg_enemy_t enemy) {
  return enemy == H2_XIANXIA_RPG_ENEMY_CORE_SHADE ||
         enemy == H2_XIANXIA_RPG_ENEMY_NODE_ECHO ||
         enemy == H2_XIANXIA_RPG_ENEMY_BRIDGE_SHADE ||
         enemy == H2_XIANXIA_RPG_ENEMY_MIRROR ||
         enemy == H2_XIANXIA_RPG_ENEMY_WENZHOU ||
         enemy == H2_XIANXIA_RPG_ENEMY_INK_TIDE ||
         enemy == H2_XIANXIA_RPG_ENEMY_DRAGON ||
         enemy == H2_XIANXIA_RPG_ENEMY_SWORD_SOUL ||
         enemy == H2_XIANXIA_RPG_ENEMY_FOG_WRAITH;
}

static void start_fight(h2_xianxia_rpg_game_t* game,
                        h2_xianxia_rpg_enemy_t enemy, bool tutorial) {
  h2_xianxia_rpg_battle_t* b = &game->battle;
  const h2_xianxia_rpg_enemy_info_t* info = h2_xianxia_rpg_enemy_info(enemy);
  const uint32_t message_id = b->message_id;
  memset(b, 0, sizeof(*b));
  b->message_id = message_id;
  b->enemy = (uint8_t)enemy;
  b->boss = is_boss(enemy);
  b->tutorial = tutorial;
  b->no_flee = b->boss || tutorial;
  b->enemy_hp = b->enemy_hp_max = tutorial ? TUTORIAL_GHOST_HP : info->hp;
  b->enemy_attack = info->attack;
  if (enemy == H2_XIANXIA_RPG_ENEMY_TIDE_OTTER &&
      (game->flags & H2_XIANXIA_RPG_FLAG_C5_HIGH))
    b->enemy_attack = b->enemy_attack * 120u / 100u;
  b->enemy_defense = info->defense;
  b->mirror_command = H2_XIANXIA_RPG_COMMAND_GUARD;
  b->node_battle = enemy == H2_XIANXIA_RPG_ENEMY_NODE_ECHO ||
                   enemy == H2_XIANXIA_RPG_ENEMY_CORE_SHADE;
  if (b->node_battle)
    b->node_phase =
        enemy == H2_XIANXIA_RPG_ENEMY_NODE_ECHO ? game->node_room : 0u;
  if (enemy == H2_XIANXIA_RPG_ENEMY_INK_TIDE) {
    b->lamp_battle = true;
    b->lamp_hp = b->lamp_max = 80u;
    b->wave = 1u;
  }
  if (enemy == H2_XIANXIA_RPG_ENEMY_BRIDGE_SHADE) {
    b->dual_lamps = b->lamp_battle = true;
    b->lamp_hp = b->west_lamp_hp = b->lamp_max = 60u;
  }
  b->ally = tutorial || h2_xianxia_rpg_ally_joined(game);
  if (b->ally)
    b->rapport = game->trust < h2_xianxia_rpg_rapport_max(game)
                     ? game->trust
                     : h2_xianxia_rpg_rapport_max(game) - 1u;
  b->veiled = enemy == H2_XIANXIA_RPG_ENEMY_FOG_GHOST ||
              (b->node_battle && b->node_phase == 0u);
  b->state = H2_XIANXIA_RPG_BATTLE_INTRO;
  if (b->boss)
    battle_say(game, text(H2_XIANXIA_RPG_TEXT_BOSS_APPEARS), info->name);
  else
    battle_say(game,
               text(b->veiled ? H2_XIANXIA_RPG_TEXT_APPEARS_VEILED
                              : H2_XIANXIA_RPG_TEXT_APPEARS),
               info->name);
  game->moving = false;
  game->from_x = game->tx;
  game->from_y = game->ty;
  game->ally_from_x = game->ally_x;
  game->ally_from_y = game->ally_y;
  clear_path(game);
  game->dialogue.active = false;
  enter_phase(game, H2_XIANXIA_RPG_PHASE_BATTLE);
}

static void start_battle(h2_xianxia_rpg_game_t* game,
                         h2_xianxia_rpg_enemy_t enemy) {
  start_fight(game, enemy, false);
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
  *chosen = (h2_xianxia_rpg_popup_t){
      .active = true,
      .on_enemy = on_enemy,
      .heal = heal,
      .target = !on_enemy && b->lamp_battle
                    ? (heal ? (b->heal_ally ? 1u : 0u) : b->hit_target)
                    : 0u,
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

static uint32_t percent_of(uint32_t value, uint32_t percent) {
  const uint32_t scaled = value * percent / 100u;
  return scaled != 0u ? scaled : 1u;
}

static bool skill_learned(const h2_xianxia_rpg_game_t* game) {
  return game->stage >= H2_XIANXIA_RPG_STAGE_TO_SHRINE;
}

bool h2_xianxia_rpg_command_available(const h2_xianxia_rpg_game_t* game,
                                      h2_xianxia_rpg_command_t command) {
  switch (command) {
    case H2_XIANXIA_RPG_COMMAND_SKILL:
      return skill_learned(game) && !game->battle.mp_sealed &&
             game->hero.mp >= H2_XIANXIA_RPG_SKILL_MP;
    case H2_XIANXIA_RPG_COMMAND_PILL:
      return game->hero.pills > 0u;
    case H2_XIANXIA_RPG_COMMAND_FLEE:
      return game->battle.node_battle || game->battle.lamp_battle ||
             (!game->battle.boss && !game->battle.no_flee);
    case H2_XIANXIA_RPG_COMMAND_ATTACK:
    case H2_XIANXIA_RPG_COMMAND_GUARD:
      return true;
    default:
      return false;
  }
}

uint32_t h2_xianxia_rpg_c9_phase(const h2_xianxia_rpg_game_t* game) {
  return game->battle.node_phase;
}
bool h2_xianxia_rpg_c9_wenzhou(const h2_xianxia_rpg_game_t* game) {
  return (game->facts & H2_XIANXIA_RPG_FACT_C9_ACTIVE_WEN) != 0u;
}
static const char* c9_text(h2_xianxia_rpg_c9_text_id_t id) {
  return h2_xianxia_rpg_c9_text[id];
}

uint32_t h2_xianxia_rpg_rapport_max(const h2_xianxia_rpg_game_t* game) {
  return game->facts & H2_XIANXIA_RPG_FACT_C6_HEALED
             ? 4u
             : H2_XIANXIA_RPG_RAPPORT_MAX;
}

bool h2_xianxia_rpg_joint_ready(const h2_xianxia_rpg_game_t* game) {
  const h2_xianxia_rpg_battle_t* b = &game->battle;
  return b->ally && (!b->lamp_battle || game->ally_hp > 0u) &&
         b->rapport >= h2_xianxia_rpg_rapport_max(game);
}

h2_xianxia_rpg_ally_action_t h2_xianxia_rpg_ally_intent(
    const h2_xianxia_rpg_game_t* game) {
  const h2_xianxia_rpg_battle_t* b = &game->battle;
  if (!b->ally) return H2_XIANXIA_RPG_ALLY_NONE;
  return b->veiled || b->enemy == H2_XIANXIA_RPG_ENEMY_MIRROR
             ? H2_XIANXIA_RPG_ALLY_LANTERN
             : H2_XIANXIA_RPG_ALLY_POLE;
}

static void gain_rapport(h2_xianxia_rpg_game_t* game) {
  h2_xianxia_rpg_battle_t* b = &game->battle;
  if (!b->ally || b->rapport >= h2_xianxia_rpg_rapport_max(game)) return;
  ++b->rapport;
  battle_append(game, " %s", text(H2_XIANXIA_RPG_TEXT_RAPPORT_UP));
}

/* The teaching fight keeps its enemy alive until a joint strike lands. */
static void hold_tutorial_enemy(h2_xianxia_rpg_game_t* game) {
  h2_xianxia_rpg_battle_t* b = &game->battle;
  if (!b->tutorial || b->joint || !b->hit_on_enemy) return;
  uint32_t total = 0;
  for (uint32_t i = 0; i < b->hit_count; ++i) total += b->hits[i];
  if (total < b->enemy_hp) return;
  uint32_t room = b->enemy_hp > 0u ? b->enemy_hp - 1u : 0u;
  for (uint32_t i = 0; i < b->hit_count; ++i) {
    if (b->hits[i] > room) b->hits[i] = room;
    room -= b->hits[i];
  }
  battle_append(game, "%s", text(H2_XIANXIA_RPG_TEXT_TUTORIAL_HOLD));
}

static uint32_t hits_total(const h2_xianxia_rpg_battle_t* b) {
  uint32_t total = 0;
  for (uint32_t i = 0; i < b->hit_count; ++i) total += b->hits[i];
  return total;
}

/* Reduce one blow at the hero by guarding and the companion's bracing; the
 * teaching fight never lets the hero fall. */
static uint32_t soften(const h2_xianxia_rpg_game_t* game, uint32_t damage) {
  const h2_xianxia_rpg_battle_t* b = &game->battle;
  if (b->guarding) damage = (damage * 2u + 4u) / 5u;
  if (b->steadied) damage = damage * STEADY_PERCENT / 100u;
  return damage != 0u ? damage : 1u;
}

static void hold_tutorial_hero(h2_xianxia_rpg_game_t* game) {
  h2_xianxia_rpg_battle_t* b = &game->battle;
  if (!b->tutorial) return;
  uint32_t room = game->hero.hp > 0u ? game->hero.hp - 1u : 0u;
  for (uint32_t i = 0; i < b->hit_count; ++i) {
    if (b->hits[i] > room) b->hits[i] = room;
    room -= b->hits[i];
  }
}

static void enemy_strike(h2_xianxia_rpg_game_t* game, uint32_t multiplier,
                         uint32_t count) {
  h2_xianxia_rpg_battle_t* b = &game->battle;
  for (uint32_t i = 0; i < count && i < H2_XIANXIA_RPG_MAX_HITS; ++i)
    b->hits[i] = soften(game, roll_damage(game, b->enemy_attack, multiplier,
                                          15u, game->hero.defense));
  b->hit_count =
      count < H2_XIANXIA_RPG_MAX_HITS ? count : H2_XIANXIA_RPG_MAX_HITS;
  hold_tutorial_hero(game);
}

/* The wraith repeats a fixed, learnable pattern. Phase one: veil, strike,
 * gather the mist lock, release it. Below half health it splits into two
 * shadows, one real, and alternates strike, gather and release while the
 * shadows trade places after every action. */
static void wraith_turn(h2_xianxia_rpg_game_t* game) {
  h2_xianxia_rpg_battle_t* b = &game->battle;
  if (!b->phase_two && b->enemy_hp * 2u < b->enemy_hp_max) {
    b->phase_two = true;
    b->split = true;
    b->veiled = false;
    b->charging = false;
    b->steps = 0;
    b->real_shadow = (uint8_t)random_below(game, 2u);
    battle_append(game, "%s", text(H2_XIANXIA_RPG_TEXT_WRAITH_SPLIT));
    return;
  }
  const uint32_t step = b->steps++;
  const uint32_t beat = b->phase_two ? step % 3u + 1u : step % 4u;
  switch (beat) {
    case 0:
      b->veiled = true;
      battle_append(game, "%s", text(H2_XIANXIA_RPG_TEXT_WRAITH_VEIL));
      break;
    case 2:
      b->charging = true;
      battle_append(game, "%s", text(H2_XIANXIA_RPG_TEXT_WRAITH_CHARGE));
      break;
    case 3:
      if (b->charging) {
        b->charging = false;
        b->special = true;
        enemy_strike(game, 220u, 1u);
        b->mp_sealed = true;
        battle_append(game, text(H2_XIANXIA_RPG_TEXT_WRAITH_LOCK),
                      (unsigned)hits_total(b));
        break;
      }
      /* A broken charge leaves only an ordinary blow. */
      enemy_strike(game, 100u, 1u);
      battle_append(game, text(H2_XIANXIA_RPG_TEXT_WRAITH_ATTACK),
                    (unsigned)hits_total(b));
      break;
    default:
      enemy_strike(game, 100u, 1u);
      battle_append(game, text(H2_XIANXIA_RPG_TEXT_WRAITH_ATTACK),
                    (unsigned)hits_total(b));
      break;
  }
  if (b->guarding && b->hit_count > 0u)
    battle_append(game, "%s", text(H2_XIANXIA_RPG_TEXT_GUARDED));
  if (b->phase_two) {
    b->split = true;
    b->real_shadow = (uint8_t)random_below(game, 2u);
    battle_append(game, "%s", text(H2_XIANXIA_RPG_TEXT_WRAITH_SHIFT));
  }
}

/* The stance is visible for a whole command. Correct play needs no drops:
 * rain guard fills rapport, thrust is broken by a joint, wind permits a
 * second guard to recover MP. Blind attacks provoke wind's counter. */
static void sword_turn(h2_xianxia_rpg_game_t* game) {
  h2_xianxia_rpg_battle_t* b = &game->battle;
  switch (b->sword_stance) {
    case 0:
      enemy_strike(game, 90u, 3u);
      if (b->guarding) b->rapport = h2_xianxia_rpg_rapport_max(game);
      battle_say(game,
                 text(b->guarding ? H2_XIANXIA_RPG_TEXT_C3_RAIN_GUARD
                                  : H2_XIANXIA_RPG_TEXT_C3_RAIN_HIT),
                 (unsigned)hits_total(b));
      break;
    case 1:
      if (b->charging) {
        enemy_strike(game, 260u, 1u);
        battle_say(game, text(H2_XIANXIA_RPG_TEXT_C3_THRUST_HIT),
                   (unsigned)hits_total(b));
      } else {
        battle_say(game, "%s", text(H2_XIANXIA_RPG_TEXT_C3_THRUST_BREAK));
      }
      break;
    default:
      if (b->sword_counter) {
        enemy_strike(game, 250u, 1u);
        battle_say(game, text(H2_XIANXIA_RPG_TEXT_C3_COUNTER),
                   (unsigned)hits_total(b));
      } else {
        battle_say(game, "%s", text(H2_XIANXIA_RPG_TEXT_C3_WIND_WAIT));
      }
      break;
  }
  b->special = b->hit_count != 0u;
}

uint32_t h2_xianxia_rpg_lamp_target(const h2_xianxia_rpg_game_t* game) {
  return game->battle.wave_turn == 1u ? (game->ally_hp > 0u ? 1u : 0u) : 2u;
}

static void lamp_enemy_turn(h2_xianxia_rpg_game_t* game) {
  h2_xianxia_rpg_battle_t* b = &game->battle;
  const uint32_t target = h2_xianxia_rpg_lamp_target(game);
  uint32_t damage = b->wave_turn == 2u ? 40u : target == 2u ? 20u : 38u;
  b->hit_target = (uint8_t)target;
  h2_xianxia_rpg_text_id_t message = H2_XIANXIA_RPG_TEXT_C4_HIT_LAMP;
  if (target == 2u && b->lamp_guard) {
    b->hit_target = 0u;
    damage = damage / 2u;
    message = H2_XIANXIA_RPG_TEXT_C4_BLOCK;
  } else if (target != 2u) {
    if (b->guarding) damage /= 2u;
    message = target == 1u ? H2_XIANXIA_RPG_TEXT_C4_HIT_ALLY
                           : H2_XIANXIA_RPG_TEXT_C4_HIT_HERO;
  }
  b->hits[0] = damage;
  b->hit_count = 1u;
  b->special = b->wave_turn == 2u;
  battle_say(game, text(message), (unsigned)damage);
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
  b->hit_target = 0u;
  if (b->node_battle) {
    const uint32_t phase = h2_xianxia_rpg_c9_phase(game);
    const bool wen = h2_xianxia_rpg_c9_wenzhou(game);
    if (phase == 3u) {
      if (b->mirror_command == H2_XIANXIA_RPG_COMMAND_GUARD ||
          b->mirror_command == H2_XIANXIA_RPG_COMMAND_FLEE) {
        battle_say(game, "%s", text(H2_XIANXIA_RPG_TEXT_C7_M_WAIT));
      } else if (b->mirror_command == H2_XIANXIA_RPG_COMMAND_PILL) {
        uint32_t room = b->enemy_hp_max - b->enemy_hp;
        uint32_t heal = room < 24u ? room : 24u;
        b->enemy_hp += heal;
        battle_say(game, text(H2_XIANXIA_RPG_TEXT_C7_M_HEAL), (unsigned)heal);
      } else {
        enemy_strike(game, wen ? 70u : 180u, 1u);
        battle_say(game, text(H2_XIANXIA_RPG_TEXT_C7_M_HIT),
                   h2_xianxia_rpg_command_label(
                       (h2_xianxia_rpg_command_t)b->mirror_command),
                   (unsigned)hits_total(b));
      }
    } else {
      const uint32_t power = phase == 1u
                                 ? (wen ? 65u : (b->turn % 2u ? 200u : 120u))
                             : phase == 2u ? (wen ? 150u : 75u)
                                           : 80u;
      enemy_strike(game, power, 1u);
      battle_say(game, c9_text(H2_XIANXIA_RPG_C9_ENEMY_HIT),
                 (unsigned)hits_total(b));
    }
    return;
  }
  if (b->enemy == H2_XIANXIA_RPG_ENEMY_MIRROR) {
    if (b->mirror_command == H2_XIANXIA_RPG_COMMAND_GUARD) {
      battle_say(game, "%s", text(H2_XIANXIA_RPG_TEXT_C7_M_WAIT));
    } else if (b->mirror_command == H2_XIANXIA_RPG_COMMAND_PILL) {
      const uint32_t room = b->enemy_hp_max - b->enemy_hp;
      const uint32_t heal = room < 24u ? room : 24u;
      b->enemy_hp += heal;
      battle_say(game, text(H2_XIANXIA_RPG_TEXT_C7_M_HEAL), (unsigned)heal);
    } else {
      const bool skill = b->mirror_command == H2_XIANXIA_RPG_COMMAND_SKILL;
      enemy_strike(game, skill ? (b->mirror_joint ? 160u : 75u) : 100u,
                   skill ? (b->mirror_joint ? 2u : 3u) : 1u);
      battle_say(game, text(H2_XIANXIA_RPG_TEXT_C7_M_HIT),
                 skill && b->mirror_joint
                     ? text(H2_XIANXIA_RPG_TEXT_JOINT)
                     : h2_xianxia_rpg_command_label(
                           (h2_xianxia_rpg_command_t)b->mirror_command),
                 (unsigned)hits_total(b));
    }
    return;
  }
  if (b->dual_lamps) {
    uint32_t damage = b->turn % 3u == 2u ? 32u : 44u;
    if (game->facts & H2_XIANXIA_RPG_FACT_C8_WEST_FIRST)
      damage = damage * 85u / 100u;
    if (game->facts & H2_XIANXIA_RPG_FACT_C8_ACTIVE_WEN) damage -= 8u;
    if (b->lamp_guard) {
      damage /= 2u;
      b->hit_target = 0u;
      b->hits[0] = damage;
      b->hit_count = 1u;
      battle_say(game, text(H2_XIANXIA_RPG_TEXT_C8_BLOCK_HIT),
                 (unsigned)damage);
    } else {
      b->hit_target = (uint8_t)(2u + b->turn % 3u);
      b->hits[0] = damage;
      b->hit_count = 1u;
      battle_say(game, text(H2_XIANXIA_RPG_TEXT_C8_LAMP_HIT),
                 text(b->turn % 3u == 0u   ? H2_XIANXIA_RPG_TEXT_C8_EAST
                      : b->turn % 3u == 1u ? H2_XIANXIA_RPG_TEXT_C8_WEST
                                           : H2_XIANXIA_RPG_TEXT_C8_GUARD),
                 (unsigned)damage);
    }
    return;
  }
  if (b->lamp_battle) {
    lamp_enemy_turn(game);
    return;
  }
  if (b->enemy == H2_XIANXIA_RPG_ENEMY_SWORD_SOUL ||
      b->enemy == H2_XIANXIA_RPG_ENEMY_WENZHOU) {
    sword_turn(game);
    return;
  }
  if (b->enemy == H2_XIANXIA_RPG_ENEMY_FOG_WRAITH) {
    wraith_turn(game);
    return;
  }
  if (b->enemy == H2_XIANXIA_RPG_ENEMY_FOG_GHOST && !b->veiled &&
      !b->tutorial && random_below(game, 100u) < GHOST_VEIL_PERCENT) {
    b->veiled = true;
    battle_append(game, text(H2_XIANXIA_RPG_TEXT_ENEMY_VEIL), info->name);
    return;
  }
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
  if (b->special && b->enemy == H2_XIANXIA_RPG_ENEMY_REED_OTTER) {
    enemy_strike(game, 70u, 2u);
    battle_append(game, text(H2_XIANXIA_RPG_TEXT_ENEMY_DOUBLE), info->name,
                  info->special, (unsigned)hits_total(b));
  } else if (b->special && b->enemy == H2_XIANXIA_RPG_ENEMY_REED_SPRITE) {
    enemy_strike(game, 100u, 1u);
    b->entangled = true;
    battle_append(game, text(H2_XIANXIA_RPG_TEXT_ENEMY_ENTANGLE), info->name,
                  info->special, (unsigned)hits_total(b));
  } else {
    enemy_strike(game, multiplier, 1u);
    const unsigned damage = (unsigned)hits_total(b);
    if (b->special)
      battle_append(game, text(H2_XIANXIA_RPG_TEXT_ENEMY_SPECIAL), info->name,
                    info->special, damage);
    else if (b->boss)
      battle_append(game, text(H2_XIANXIA_RPG_TEXT_BOSS_TAIL), damage);
    else
      battle_append(game, text(H2_XIANXIA_RPG_TEXT_ENEMY_ATTACK), info->name,
                    damage);
  }
  if (b->guarding) battle_append(game, "%s", text(H2_XIANXIA_RPG_TEXT_GUARDED));
}

/* The companion acts after the hero: she lifts her lantern on a veiled
 * enemy, braces a guarding hero, and otherwise strikes with her pole. */
static void ally_turn(h2_xianxia_rpg_game_t* game) {
  h2_xianxia_rpg_battle_t* b = &game->battle;
  const h2_xianxia_rpg_enemy_info_t* info =
      h2_xianxia_rpg_enemy_info((h2_xianxia_rpg_enemy_t)b->enemy);
  set_battle_state(game, H2_XIANXIA_RPG_BATTLE_ALLY_ACT);
  b->hit_count = b->hits_applied = 0;
  b->hit_on_enemy = true;
  b->special = false;
  b->joint = false;
  if (b->node_battle) {
    const uint32_t phase = h2_xianxia_rpg_c9_phase(game);
    const bool wen = h2_xianxia_rpg_c9_wenzhou(game);
    b->ally_action = H2_XIANXIA_RPG_ALLY_LANTERN;
    if (phase == 0u && !wen) {
      b->veiled = false;
      battle_say(game, "%s", c9_text(H2_XIANXIA_RPG_C9_MIST_LIGHT));
    } else if (phase == 1u && wen) {
      battle_say(game, "%s", c9_text(H2_XIANXIA_RPG_C9_TIDE_CUT));
    } else if (phase == 2u && !wen) {
      const uint32_t room = game->hero.hp_max - game->hero.hp;
      game->hero.hp += room < 32u ? room : 32u;
      battle_say(game, "%s", c9_text(H2_XIANXIA_RPG_C9_COLD_WARM));
    } else if (phase == 3u && wen) {
      battle_say(game, "%s", c9_text(H2_XIANXIA_RPG_C9_MIRROR_READ));
    } else {
      b->ally_action = H2_XIANXIA_RPG_ALLY_POLE;
      b->hits[0] = b->veiled ? 3u : 12u;
      b->hit_count = 1u;
      battle_say(game, c9_text(H2_XIANXIA_RPG_C9_ALLY_HIT),
                 (unsigned)b->hits[0]);
    }
    gain_rapport(game);
    return;
  }
  if (b->dual_lamps) {
    b->ally_action = H2_XIANXIA_RPG_ALLY_LANTERN;
    if (game->facts & H2_XIANXIA_RPG_FACT_C8_ACTIVE_WEN) {
      battle_say(game, "%s", text(H2_XIANXIA_RPG_TEXT_C8_COVER));
    } else {
      uint32_t* hp =
          b->lamp_hp <= b->west_lamp_hp ? &b->lamp_hp : &b->west_lamp_hp;
      const uint32_t healed = b->lamp_max - *hp < 8u ? b->lamp_max - *hp : 8u;
      *hp += healed;
      battle_say(game, text(H2_XIANXIA_RPG_TEXT_C8_MEND), (unsigned)healed);
    }
    gain_rapport(game);
    return;
  }
  if (b->lamp_battle && game->ally_hp == 0u) {
    battle_say(game, "%s", text(H2_XIANXIA_RPG_TEXT_C4_REST_ALLY));
    return;
  }
  if (b->lamp_battle && b->ally_mend) {
    const uint32_t healed =
        b->lamp_max - b->lamp_hp < 12u ? b->lamp_max - b->lamp_hp : 12u;
    b->lamp_hp += healed;
    b->ally_action = H2_XIANXIA_RPG_ALLY_LANTERN;
    battle_say(game, text(H2_XIANXIA_RPG_TEXT_C4_REPAIR), (unsigned)healed);
    gain_rapport(game);
    return;
  }
  if (b->veiled) {
    b->ally_action = H2_XIANXIA_RPG_ALLY_LANTERN;
    b->veiled = false;
    battle_say(game, text(H2_XIANXIA_RPG_TEXT_ALLY_LANTERN), info->name);
    gain_rapport(game);
  } else if (b->guarding && !b->lamp_battle) {
    b->ally_action = H2_XIANXIA_RPG_ALLY_STEADY;
    b->steadied = true;
    battle_say(game, "%s", text(H2_XIANXIA_RPG_TEXT_ALLY_STEADY));
    gain_rapport(game);
  } else if (b->enemy == H2_XIANXIA_RPG_ENEMY_MIRROR) {
    b->ally_action = H2_XIANXIA_RPG_ALLY_LANTERN;
    battle_say(game, "%s", text(H2_XIANXIA_RPG_TEXT_C7_M_STILL));
  } else if ((b->enemy == H2_XIANXIA_RPG_ENEMY_SWORD_SOUL ||
              b->enemy == H2_XIANXIA_RPG_ENEMY_WENZHOU) &&
             b->sword_stance == 2u) {
    b->ally_action = H2_XIANXIA_RPG_ALLY_POLE;
    battle_say(game, "%s", text(H2_XIANXIA_RPG_TEXT_C3_POLE_WAIT));
  } else {
    b->ally_action = H2_XIANXIA_RPG_ALLY_POLE;
    b->hits[0] = roll_damage(game, game->hero.attack, POLE_PERCENT, 10u,
                             b->enemy_defense);
    b->hit_count = 1;
    battle_say(game, text(H2_XIANXIA_RPG_TEXT_ALLY_POLE), info->name,
               (unsigned)b->hits[0]);
    hold_tutorial_enemy(game);
  }
}

static void victory(h2_xianxia_rpg_game_t* game) {
  h2_xianxia_rpg_battle_t* b = &game->battle;
  h2_xianxia_rpg_hero_t* hero = &game->hero;
  const h2_xianxia_rpg_enemy_info_t* info =
      h2_xianxia_rpg_enemy_info((h2_xianxia_rpg_enemy_t)b->enemy);
  set_battle_state(game, H2_XIANXIA_RPG_BATTLE_VICTORY);
  b->split = false;
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
  /* A reload after a story fight keeps it won. */
  switch (b->enemy) {
    case H2_XIANXIA_RPG_ENEMY_NODE_ECHO:
      game->flags |= 1u << (4u + game->node_room);
      if ((game->flags & 240u) == 240u)
        advance_stage(game, H2_XIANXIA_RPG_STAGE_C9_FATHER);
      rest_here(game);
      battle_say(game, "%s", c9_text(H2_XIANXIA_RPG_C9_TRIAL_WON));
      break;
    case H2_XIANXIA_RPG_ENEMY_CORE_SHADE:
      game->facts |= H2_XIANXIA_RPG_FACT_C9_CLEAR;
      advance_stage(game, H2_XIANXIA_RPG_STAGE_C9_REVEAL);
      rest_here(game);
      battle_say(game, "%s", c9_text(H2_XIANXIA_RPG_C9_CORE_WON));
      break;
    case H2_XIANXIA_RPG_ENEMY_BRIDGE_SHADE:
      game->facts |= H2_XIANXIA_RPG_FACT_C8_SNOW;
      advance_stage(game, H2_XIANXIA_RPG_STAGE_C8_REVEAL);
      rest_here(game);
      battle_say(game, "%s", text(H2_XIANXIA_RPG_TEXT_C8_WON));
      break;
    case H2_XIANXIA_RPG_ENEMY_MIRROR:
      game->facts |= H2_XIANXIA_RPG_FACT_C7_MIRROR;
      advance_stage(game, H2_XIANXIA_RPG_STAGE_C7_REVEAL);
      rest_here(game);
      break;
    case H2_XIANXIA_RPG_ENEMY_WENZHOU:
      advance_stage(game, H2_XIANXIA_RPG_STAGE_C5_RESCUE);
      game->flags = (game->flags | H2_XIANXIA_RPG_FLAG_C5_HIGH) &
                    ~H2_XIANXIA_RPG_C5_STEPS_MASK;
      place_hero(game, H2_XIANXIA_RPG_MAP_OLD_CITY, 3, 4);
      rest_here(game);
      battle_say(game, "%s", text(H2_XIANXIA_RPG_TEXT_C5_WON));
      break;
    case H2_XIANXIA_RPG_ENEMY_INK_TIDE:
      if (game->ally_hp == 0u) game->ally_hp = 1u;
      advance_stage(game, H2_XIANXIA_RPG_STAGE_C4_RETURN);
      battle_say(game, "%s", text(H2_XIANXIA_RPG_TEXT_C4_WON));
      request_save(game);
      break;
    case H2_XIANXIA_RPG_ENEMY_DRAGON:
      advance_stage(game, H2_XIANXIA_RPG_STAGE_RETURN);
      request_save(game);
      break;
    case H2_XIANXIA_RPG_ENEMY_SWORD_SOUL:
      advance_stage(game, H2_XIANXIA_RPG_STAGE_C3_RETURN);
      request_save(game);
      break;
    case H2_XIANXIA_RPG_ENEMY_FOG_WRAITH:
      advance_stage(game, H2_XIANXIA_RPG_STAGE_C2_RETURN);
      game->flags |= H2_XIANXIA_RPG_FLAG_C2_HAND_LIGHT;
      request_save(game);
      break;
    case H2_XIANXIA_RPG_ENEMY_WRECK_GUARD:
      game->flags |= H2_XIANXIA_RPG_FLAG_C2_GUARD;
      request_save(game);
      break;
    default:
      break;
  }
}

/* Waves end after three enemy actions or when the foe falls. */
static void lamp_wave_end(h2_xianxia_rpg_game_t* game) {
  h2_xianxia_rpg_battle_t* b = &game->battle;
  if (!b->lamp_battle || b->dual_lamps || b->wave == 3u) {
    victory(game);
    return;
  }
  ++b->wave;
  b->wave_turn = 0u;
  b->enemy_hp = b->enemy_hp_max;
  b->hit_count = b->hits_applied = 0u;
  b->guarding = b->lamp_guard = b->steadied = false;
  set_battle_state(game, H2_XIANXIA_RPG_BATTLE_COMMAND);
  battle_say(game, text(H2_XIANXIA_RPG_TEXT_C4_WAVE), (unsigned)b->wave);
}

static void strike_with(h2_xianxia_rpg_game_t* game,
                        h2_xianxia_rpg_command_t command);

static void hero_command(h2_xianxia_rpg_game_t* game,
                         h2_xianxia_rpg_command_t command) {
  h2_xianxia_rpg_battle_t* b = &game->battle;
  const h2_xianxia_rpg_enemy_info_t* info =
      h2_xianxia_rpg_enemy_info((h2_xianxia_rpg_enemy_t)b->enemy);
  if (!h2_xianxia_rpg_command_available(game, command)) {
    if (command == H2_XIANXIA_RPG_COMMAND_SKILL)
      battle_say(game, "%s",
                 text(!skill_learned(game) ? H2_XIANXIA_RPG_TEXT_NO_SKILL
                      : b->mp_sealed       ? H2_XIANXIA_RPG_TEXT_SEALED
                                           : H2_XIANXIA_RPG_TEXT_NO_MP));
    else if (command == H2_XIANXIA_RPG_COMMAND_PILL)
      battle_say(game, "%s", text(H2_XIANXIA_RPG_TEXT_NO_PILLS));
    else if (command == H2_XIANXIA_RPG_COMMAND_FLEE)
      battle_say(game, text(H2_XIANXIA_RPG_TEXT_NO_FLEE), info->name);
    return;
  }
  if (b->lamp_battle && !b->dual_lamps &&
      command == H2_XIANXIA_RPG_COMMAND_PILL) {
    set_battle_state(game, H2_XIANXIA_RPG_BATTLE_HEAL_TARGET);
    battle_say(game, "%s", text(H2_XIANXIA_RPG_TEXT_C4_HEAL));
    return;
  }
  /* Against two shadows a strike waits for its target. */
  if (b->split && (command == H2_XIANXIA_RPG_COMMAND_ATTACK ||
                   command == H2_XIANXIA_RPG_COMMAND_SKILL)) {
    b->pending = (uint8_t)command;
    set_battle_state(game, H2_XIANXIA_RPG_BATTLE_TARGET);
    return;
  }
  strike_with(game, command);
}

static void strike_with(h2_xianxia_rpg_game_t* game,
                        h2_xianxia_rpg_command_t command) {
  h2_xianxia_rpg_battle_t* b = &game->battle;
  h2_xianxia_rpg_hero_t* hero = &game->hero;
  const h2_xianxia_rpg_enemy_info_t* info =
      h2_xianxia_rpg_enemy_info((h2_xianxia_rpg_enemy_t)b->enemy);
  b->command = (uint8_t)command;
  b->hit_count = b->hits_applied = 0;
  b->hit_on_enemy = true;
  b->special = false;
  b->joint = false;
  b->ally_action = H2_XIANXIA_RPG_ALLY_NONE;
  ++game->turns_taken;
  set_battle_state(game, H2_XIANXIA_RPG_BATTLE_HERO_ACT);
  const bool hindered =
      b->entangled && (command == H2_XIANXIA_RPG_COMMAND_ATTACK ||
                       command == H2_XIANXIA_RPG_COMMAND_SKILL);
  switch (command) {
    case H2_XIANXIA_RPG_COMMAND_ATTACK: {
      const bool critical = random_below(game, 100u) < CRIT_PERCENT;
      uint32_t damage = roll_damage(game, hero->attack, critical ? 160u : 100u,
                                    10u, b->enemy_defense);
      if (hindered) damage = percent_of(damage, HINDER_PERCENT);
      if (b->veiled) damage = percent_of(damage, VEIL_PERCENT);
      b->hits[0] = damage;
      b->hit_count = 1;
      b->special = critical;
      if (b->veiled)
        battle_say(game, text(H2_XIANXIA_RPG_TEXT_VEILED_HIT), info->name,
                   (unsigned)damage);
      else
        battle_say(game,
                   text(critical ? H2_XIANXIA_RPG_TEXT_CRITICAL
                                 : H2_XIANXIA_RPG_TEXT_ATTACK),
                   info->name, (unsigned)damage);
      if (hindered)
        battle_append(game, "%s", text(H2_XIANXIA_RPG_TEXT_HINDERED));
      if (!b->veiled) gain_rapport(game);
      break;
    }
    case H2_XIANXIA_RPG_COMMAND_SKILL: {
      hero->mp -= H2_XIANXIA_RPG_SKILL_MP;
      if (h2_xianxia_rpg_joint_ready(game)) {
        /* Sword and lantern together: pierces the veil, breaks a charge. */
        b->joint = b->joint_landed = true;
        b->rapport = 0;
        b->hits[0] = roll_damage(game, hero->attack, JOINT_PERCENT_A, 8u,
                                 b->enemy_defense);
        b->hits[1] = roll_damage(game, hero->attack, JOINT_PERCENT_B, 8u,
                                 b->enemy_defense);
        b->hit_count = 2;
        b->special = true;
        b->veiled = false;
        battle_say(game, text(H2_XIANXIA_RPG_TEXT_JOINT_STRIKE),
                   (unsigned)hits_total(b));
        if (b->charging) {
          b->charging = false;
          battle_append(game, "%s", text(H2_XIANXIA_RPG_TEXT_JOINT_BREAK));
        }
        break;
      }
      uint32_t total = 0;
      for (uint32_t i = 0; i < H2_XIANXIA_RPG_MAX_HITS; ++i) {
        uint32_t damage =
            roll_damage(game, hero->attack, 75u, 8u, b->enemy_defense);
        if (hindered) damage = percent_of(damage, HINDER_PERCENT);
        if (b->veiled) damage = percent_of(damage, VEIL_PERCENT);
        b->hits[i] = damage;
        total += damage;
      }
      b->hit_count = H2_XIANXIA_RPG_MAX_HITS;
      b->special = true;
      battle_say(game,
                 text(b->veiled ? H2_XIANXIA_RPG_TEXT_VEILED_SKILL
                                : H2_XIANXIA_RPG_TEXT_SKILL),
                 (unsigned)total);
      if (hindered)
        battle_append(game, "%s", text(H2_XIANXIA_RPG_TEXT_HINDERED));
      if (!b->veiled) gain_rapport(game);
      break;
    }
    case H2_XIANXIA_RPG_COMMAND_PILL: {
      --hero->pills;
      const uint32_t heal_max = 40u + 8u * hero->level;
      uint32_t* hp =
          b->lamp_battle && b->heal_ally ? &game->ally_hp : &hero->hp;
      const uint32_t max =
          b->lamp_battle && b->heal_ally ? game->ally_hp_max : hero->hp_max;
      const uint32_t room = max - *hp;
      const uint32_t healed = heal_max < room ? heal_max : room;
      *hp += healed;
      popup(game, false, true, healed, 0);
      if (b->lamp_battle)
        battle_say(game, text(H2_XIANXIA_RPG_TEXT_C4_PILL),
                   h2_xianxia_rpg_actor_name(b->heal_ally
                                                 ? H2_XIANXIA_RPG_ACTOR_JIANGLI
                                                 : H2_XIANXIA_RPG_ACTOR_HERO),
                   (unsigned)healed);
      else
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
      if (b->node_battle) {
        game->facts ^= H2_XIANXIA_RPG_FACT_C9_ACTIVE_WEN;
        battle_say(
            game, c9_text(H2_XIANXIA_RPG_C9_SWITCHED),
            h2_xianxia_rpg_actor_name(h2_xianxia_rpg_c9_wenzhou(game)
                                          ? H2_XIANXIA_RPG_ACTOR_WENZHOU
                                          : H2_XIANXIA_RPG_ACTOR_JIANGLI));
        break;
      }
      if (b->lamp_battle) {
        b->lamp_guard = true;
        hero->mp = hero->mp + GUARD_MP < hero->mp_max ? hero->mp + GUARD_MP
                                                      : hero->mp_max;
        battle_say(game, "%s", text(H2_XIANXIA_RPG_TEXT_C4_GUARDING));
        gain_rapport(game);
        break;
      }
      if (random_below(game, 100u) < FLEE_PERCENT) {
        set_battle_state(game, H2_XIANXIA_RPG_BATTLE_FLED);
        battle_say(game, "%s", text(H2_XIANXIA_RPG_TEXT_FLED));
      } else {
        battle_say(game, "%s", text(H2_XIANXIA_RPG_TEXT_FLEE_FAILED));
      }
      break;
  }
  if ((b->enemy == H2_XIANXIA_RPG_ENEMY_SWORD_SOUL ||
       b->enemy == H2_XIANXIA_RPG_ENEMY_WENZHOU) &&
      b->sword_stance == 2u &&
      (command == H2_XIANXIA_RPG_COMMAND_ATTACK ||
       command == H2_XIANXIA_RPG_COMMAND_SKILL)) {
    b->sword_counter = true;
    b->hit_count = 0u;
    battle_say(game, "%s", text(H2_XIANXIA_RPG_TEXT_C3_REBUFF));
  }
  b->chosen_joint = b->joint;
  if (hindered) b->entangled = false;
  /* The mist lock lasts for one command. */
  b->mp_sealed = false;
  hold_tutorial_enemy(game);
}

/* A tap on a shadow resolves the waiting strike. The false one bursts and
 * feeds the real one, and the hero's turn is spent. */
static void choose_shadow(h2_xianxia_rpg_game_t* game, uint32_t shadow) {
  h2_xianxia_rpg_battle_t* b = &game->battle;
  if (shadow == (uint32_t)b->real_shadow) {
    strike_with(game, (h2_xianxia_rpg_command_t)b->pending);
    return;
  }
  b->command = H2_XIANXIA_RPG_COMMAND_ATTACK;
  b->hit_count = b->hits_applied = 0;
  b->hit_on_enemy = true;
  b->special = false;
  b->joint = false;
  b->split = false;
  ++game->turns_taken;
  const uint32_t heal = b->enemy_hp_max * WRONG_SHADOW_HEAL_PERCENT / 100u;
  const uint32_t room = b->enemy_hp_max - b->enemy_hp;
  const uint32_t healed = heal < room ? heal : room;
  b->enemy_hp += healed;
  b->mp_sealed = false;
  set_battle_state(game, H2_XIANXIA_RPG_BATTLE_HERO_ACT);
  battle_say(game, text(H2_XIANXIA_RPG_TEXT_WRONG_SHADOW), (unsigned)healed);
}

static uint32_t act_duration(const h2_xianxia_rpg_battle_t* b) {
  const uint32_t extra =
      b->hit_count > 1u ? (b->hit_count - 1u) * H2_XIANXIA_RPG_HIT_GAP_MS : 0u;
  return (b->state == H2_XIANXIA_RPG_BATTLE_ALLY_ACT
              ? H2_XIANXIA_RPG_ALLY_ACT_MS
              : H2_XIANXIA_RPG_ACT_MS) +
         extra;
}

static void apply_hits(h2_xianxia_rpg_game_t* game) {
  h2_xianxia_rpg_battle_t* b = &game->battle;
  while (b->hits_applied < b->hit_count &&
         b->state_ms >= H2_XIANXIA_RPG_HIT_MS +
                            b->hits_applied * H2_XIANXIA_RPG_HIT_GAP_MS) {
    uint32_t damage = b->hits[b->hits_applied];
    if (b->hit_on_enemy) {
      if (b->enemy == H2_XIANXIA_RPG_ENEMY_MIRROR &&
          b->mirror_command == H2_XIANXIA_RPG_COMMAND_GUARD)
        damage = percent_of(damage, 70u);
      b->enemy_hp = damage < b->enemy_hp ? b->enemy_hp - damage : 0u;
      if (b->enemy == H2_XIANXIA_RPG_ENEMY_WENZHOU &&
          b->enemy_hp < b->enemy_hp_max / 2u)
        b->enemy_hp = b->enemy_hp_max / 2u;
      b->enemy_flash_ms = H2_XIANXIA_RPG_FLASH_MS;
    } else if (b->dual_lamps && b->hit_target >= 2u) {
      if (b->hit_target != 3u)
        b->lamp_hp = damage < b->lamp_hp ? b->lamp_hp - damage : 0u;
      if (b->hit_target != 2u)
        b->west_lamp_hp =
            damage < b->west_lamp_hp ? b->west_lamp_hp - damage : 0u;
    } else if (b->lamp_battle && b->hit_target == 1u) {
      game->ally_hp = damage < game->ally_hp ? game->ally_hp - damage : 0u;
    } else if (b->lamp_battle && b->hit_target == 2u) {
      b->lamp_hp = damage < b->lamp_hp ? b->lamp_hp - damage : 0u;
    } else {
      game->hero.hp = damage < game->hero.hp ? game->hero.hp - damage : 0u;
      b->hero_flash_ms = H2_XIANXIA_RPG_FLASH_MS;
    }
    popup(game, b->hit_on_enemy, false, damage, (int32_t)b->hits_applied);
    ++b->hits_applied;
  }
}

static bool duel_yielded(const h2_xianxia_rpg_battle_t* b) {
  return b->enemy == H2_XIANXIA_RPG_ENEMY_WENZHOU &&
         (b->enemy_hp <= b->enemy_hp_max / 2u || b->turn >= 6u);
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
        if (b->enemy_hp == 0u || duel_yielded(b))
          lamp_wave_end(game);
        else if (b->ally)
          ally_turn(game);
        else
          enemy_turn(game);
      }
      break;
    case H2_XIANXIA_RPG_BATTLE_ALLY_ACT:
      apply_hits(game);
      if (b->state_ms >= act_duration(b)) {
        if (b->enemy_hp == 0u || duel_yielded(b))
          lamp_wave_end(game);
        else
          enemy_turn(game);
      }
      break;
    case H2_XIANXIA_RPG_BATTLE_ENEMY_ACT:
      apply_hits(game);
      if (b->state_ms >= act_duration(b)) {
        b->guarding = false;
        b->lamp_guard = false;
        b->steadied = false;
        ++b->turn;
        if (b->enemy == H2_XIANXIA_RPG_ENEMY_MIRROR) {
          b->mirror_command = b->command;
          b->mirror_joint = b->chosen_joint;
        }
        if (b->node_battle) {
          b->mirror_command = b->command;
          b->mirror_joint = b->chosen_joint;
          if (b->enemy == H2_XIANXIA_RPG_ENEMY_CORE_SHADE) {
            const uint8_t next = (uint8_t)((b->turn / 2u) % 4u);
            if (next != b->node_phase) b->veiled = next == 0u;
            b->node_phase = next;
          }
        }
        if (b->lamp_battle) ++b->wave_turn;
        if (b->enemy == H2_XIANXIA_RPG_ENEMY_SWORD_SOUL ||
            b->enemy == H2_XIANXIA_RPG_ENEMY_WENZHOU) {
          b->sword_stance = (uint8_t)((b->sword_stance + 1u) % 3u);
          b->charging = b->sword_stance == 1u;
          b->sword_counter = false;
        }
        if (game->hero.hp == 0u || (b->lamp_battle && b->lamp_hp == 0u) ||
            (b->dual_lamps && b->west_lamp_hp == 0u)) {
          set_battle_state(game, H2_XIANXIA_RPG_BATTLE_DEFEAT);
          battle_say(
              game, "%s",
              text(b->dual_lamps ? (b->lamp_hp == 0u || b->west_lamp_hp == 0u
                                        ? H2_XIANXIA_RPG_TEXT_C8_LOST
                                        : H2_XIANXIA_RPG_TEXT_DEFEAT)
                   : b->lamp_battle && b->lamp_hp == 0u
                       ? H2_XIANXIA_RPG_TEXT_C4_LOST
                       : H2_XIANXIA_RPG_TEXT_DEFEAT));
        } else if (duel_yielded(b)) {
          victory(game);
        } else if (b->dual_lamps &&
                   b->turn >= ((game->facts & H2_XIANXIA_RPG_FACT_C8_WEST_FIRST)
                                   ? 8u
                                   : 6u)) {
          victory(game);
        } else if (!b->dual_lamps && b->lamp_battle && b->wave_turn >= 3u) {
          lamp_wave_end(game);
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
    if (b->enemy == H2_XIANXIA_RPG_ENEMY_CORE_SHADE)
      start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C9_REVEAL);
    else if (b->enemy == H2_XIANXIA_RPG_ENEMY_NODE_ECHO)
      start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C9_TRIAL_WON);
    else if (b->dual_lamps)
      start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C8_REVEAL);
    else if (b->enemy == H2_XIANXIA_RPG_ENEMY_MIRROR)
      start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C7_REVEAL);
    else if (b->enemy == H2_XIANXIA_RPG_ENEMY_WENZHOU)
      start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C5_FLOOD);
    else if (b->lamp_battle)
      start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C4_HOME);
    else if (b->tutorial)
      start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C2_JIANGLI_JOIN);
    else if (b->enemy == H2_XIANXIA_RPG_ENEMY_DRAGON)
      start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_VICTORY);
    else if (b->enemy == H2_XIANXIA_RPG_ENEMY_FOG_WRAITH)
      start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C2_RELIT);
    else if (b->enemy == H2_XIANXIA_RPG_ENEMY_SWORD_SOUL)
      start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C3_QUIET);
    else if (b->enemy == H2_XIANXIA_RPG_ENEMY_WRECK_GUARD)
      start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C2_GUARD_DOWN);
  } else if (b->state == H2_XIANXIA_RPG_BATTLE_DEFEAT) {
    restore_party(game);
    game->facing = H2_XIANXIA_RPG_FACING_DOWN;
    place_hero(game, game->rest_map, game->rest_x, game->rest_y);
    enter_phase(game, H2_XIANXIA_RPG_PHASE_EXPLORE);
    if (game->chapter >= 7u) request_save(game);
    start_dialogue(game, game->chapter == 9u   ? H2_XIANXIA_RPG_SCRIPT_C9_RETRY
                         : game->chapter == 8u ? H2_XIANXIA_RPG_SCRIPT_C8_RETRY
                         : game->chapter == 7u ? H2_XIANXIA_RPG_SCRIPT_C7_RETRY
                         : game->chapter == 5u ? H2_XIANXIA_RPG_SCRIPT_C5_REVIVE
                         : game->chapter == 4u ? H2_XIANXIA_RPG_SCRIPT_C4_REVIVE
                         : game->chapter == 3u ? H2_XIANXIA_RPG_SCRIPT_C3_REVIVE
                         : b->enemy == H2_XIANXIA_RPG_ENEMY_FOG_WRAITH
                             ? H2_XIANXIA_RPG_SCRIPT_C2_REVIVE_BOSS
                         : b->boss ? H2_XIANXIA_RPG_SCRIPT_REVIVE_BOSS
                                   : H2_XIANXIA_RPG_SCRIPT_REVIVE);
  } else if (b->state == H2_XIANXIA_RPG_BATTLE_FLED) {
    enter_phase(game, H2_XIANXIA_RPG_PHASE_EXPLORE);
    reset_encounter(game);
  }
}

static void command_tap(h2_xianxia_rpg_game_t* game, float x, float y) {
  for (int32_t i = 0; i < H2_XIANXIA_RPG_COMMANDS; ++i)
    if (inside(&game->layout.commands[i], x, y)) {
      if (game->battle.state == H2_XIANXIA_RPG_BATTLE_TARGET)
        set_battle_state(game, H2_XIANXIA_RPG_BATTLE_COMMAND);
      hero_command(game, (h2_xianxia_rpg_command_t)i);
      return;
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
    case H2_XIANXIA_RPG_BATTLE_TARGET:
      for (uint32_t i = 0; i < 2u; ++i)
        if (inside(&game->layout.shadows[i], x, y)) {
          choose_shadow(game, i);
          return;
        }
      /* Another command may still be chosen instead. */
      command_tap(game, x, y);
      break;
    case H2_XIANXIA_RPG_BATTLE_HEAL_TARGET:
      for (uint32_t i = 0; i < 2u; ++i) {
        if (!inside(&game->layout.party[i], x, y)) continue;
        b->heal_ally = i == 1u;
        strike_with(game, H2_XIANXIA_RPG_COMMAND_PILL);
        return;
      }
      command_tap(game, x, y);
      break;
    case H2_XIANXIA_RPG_BATTLE_COMMAND:
      if (b->lamp_battle && !b->dual_lamps &&
          inside(&game->layout.party[1], x, y)) {
        b->ally_mend = !b->ally_mend;
        battle_say(game, "%s",
                   text(game->ally_hp == 0u ? H2_XIANXIA_RPG_TEXT_C4_REST_ALLY
                        : b->ally_mend      ? H2_XIANXIA_RPG_TEXT_C4_MEND
                                            : H2_XIANXIA_RPG_TEXT_C4_POLE));
        return;
      }
      command_tap(game, x, y);
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

/* Both clues heard together earn the companion's trust once. */
static void check_trust(h2_xianxia_rpg_game_t* game) {
  const uint32_t both =
      H2_XIANXIA_RPG_FLAG_C2_FISHER | H2_XIANXIA_RPG_FLAG_C2_ROPE;
  if ((game->flags & both) != both ||
      (game->flags & H2_XIANXIA_RPG_FLAG_C2_TRUST))
    return;
  game->flags |= H2_XIANXIA_RPG_FLAG_C2_TRUST;
  if (game->trust < H2_XIANXIA_RPG_TRUST_MAX) ++game->trust;
  start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C2_TRUST);
}

static void record_clear(h2_xianxia_rpg_game_t* game) {
  if (game->chapter < 1u || game->chapter > H2_XIANXIA_RPG_CHAPTER_MAX) return;
  h2_xianxia_rpg_progress_t* p = &game->records.progress;
  p->cleared |= 1u << (game->chapter - 1u);
  p->chapters[game->chapter - 1u] = (h2_xianxia_rpg_clear_t){
      .level = game->hero.level,
      .exp = game->hero.exp,
      .pills = game->hero.pills,
      .battles = game->battles_won,
      .turns = game->turns_taken,
      .flags = game->flags,
      .trust = game->trust,
  };
  if (game->chapter == 9u)
    p->reserved[8] = (uint8_t)((game->facts >> 24) & 15u);
  if (game->chapter == 8u)
    p->reserved[7] =
        (uint8_t)(((game->facts >> 12) & 1u) | ((game->facts >> 15) & 6u) |
                  ((game->facts >> 20) & 8u) | ((game->facts >> 14) & 16u));
  if (game->chapter == 6u) p->reserved[5] = (uint8_t)game->facts;
  if (game->chapter == 7u)
    p->reserved[6] =
        (uint8_t)((game->facts >> H2_XIANXIA_RPG_C7_ANSWERS_SHIFT) & 15u);
  /* An unreadable future table is opaque, even when this chapter clears.
   * Its own journey still records the result without destroying that table. */
  game->progress_pending = !game->records.progress_unreadable;
}

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
      rest_here(game);
      break;
    case H2_XIANXIA_RPG_EFFECT_BOSS_BATTLE:
      start_battle(game, H2_XIANXIA_RPG_ENEMY_DRAGON);
      break;
    case H2_XIANXIA_RPG_EFFECT_ENDING: {
      const h2_xianxia_rpg_chapter_info_t* info =
          h2_xianxia_rpg_chapter_info(game->chapter);
      game->stage =
          info != NULL ? info->cleared_stage : H2_XIANXIA_RPG_STAGE_CLEARED;
      record_clear(game);
      /* A reload after the ending reopens the result. */
      request_save(game);
      enter_phase(game, H2_XIANXIA_RPG_PHASE_ENDING);
      break;
    }
    case H2_XIANXIA_RPG_EFFECT_C9_TRIAL:
      if (!(game->flags & (1u << (4u + game->node_room)))) {
        rest_here(game);
        start_battle(game, H2_XIANXIA_RPG_ENEMY_NODE_ECHO);
      }
      break;
    case H2_XIANXIA_RPG_EFFECT_C9_FATHER:
      game->facts |= H2_XIANXIA_RPG_FACT_C9_FATHER;
      advance_stage(game, H2_XIANXIA_RPG_STAGE_C9_BATTLE);
      rest_here(game);
      break;
    case H2_XIANXIA_RPG_EFFECT_C9_BATTLE:
      rest_here(game);
      start_battle(game, H2_XIANXIA_RPG_ENEMY_CORE_SHADE);
      break;
    case H2_XIANXIA_RPG_EFFECT_C8_BATTLE:
      start_battle(game, H2_XIANXIA_RPG_ENEMY_BRIDGE_SHADE);
      break;
    case H2_XIANXIA_RPG_EFFECT_C7_FEEDBACK:
      game->flags &= ~H2_XIANXIA_RPG_FLAG_C7_FEEDBACK;
      request_save(game);
      break;
    case H2_XIANXIA_RPG_EFFECT_C7_BATTLE:
      place_hero(game, H2_XIANXIA_RPG_MAP_MIRROR_HALL, 11, 4);
      rest_here(game);
      start_battle(game, H2_XIANXIA_RPG_ENEMY_MIRROR);
      break;
    case H2_XIANXIA_RPG_EFFECT_C6_BRIEF:
      if (game->stage == H2_XIANXIA_RPG_STAGE_C6_ARRIVE)
        advance_stage(game, H2_XIANXIA_RPG_STAGE_C6_HERBS);
      game->flags = (game->flags & ~255u) | 4u;
      rest_here(game);
      break;
    case H2_XIANXIA_RPG_EFFECT_C6_RITUAL:
      advance_stage(game, H2_XIANXIA_RPG_STAGE_C6_RITUAL);
      game->flags = (game->flags & ~255u) | 4u;
      rest_here(game);
      break;
    case H2_XIANXIA_RPG_EFFECT_C5_BRIEF:
      if (game->stage == H2_XIANXIA_RPG_STAGE_C5_ARRIVE)
        advance_stage(game, H2_XIANXIA_RPG_STAGE_C5_PAGE);
      rest_here(game);
      break;
    case H2_XIANXIA_RPG_EFFECT_C5_QUESTION:
      advance_stage(game, H2_XIANXIA_RPG_STAGE_C5_CHOICE);
      rest_here(game);
      break;
    case H2_XIANXIA_RPG_EFFECT_C5_EVIDENCE:
      advance_stage(game, H2_XIANXIA_RPG_STAGE_C5_DUEL);
      rest_here(game);
      start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C5_EVIDENCE);
      break;
    case H2_XIANXIA_RPG_EFFECT_C5_DUEL:
      start_battle(game, H2_XIANXIA_RPG_ENEMY_WENZHOU);
      break;
    case H2_XIANXIA_RPG_EFFECT_C5_END:
      apply_effect(game, H2_XIANXIA_RPG_EFFECT_ENDING);
      break;
    case H2_XIANXIA_RPG_EFFECT_C4_JOIN:
      game->flags |= H2_XIANXIA_RPG_FLAG_C4_JOINED;
      restore_party(game);
      request_save(game);
      break;
    case H2_XIANXIA_RPG_EFFECT_C4_FIRE:
      game->flags |= H2_XIANXIA_RPG_FLAG_C4_FIRE;
      if (game->stage == H2_XIANXIA_RPG_STAGE_C4_HOME)
        advance_stage(game, H2_XIANXIA_RPG_STAGE_C4_ESCORT);
      rest_here(game);
      break;
    case H2_XIANXIA_RPG_EFFECT_C4_LIGHT1:
    case H2_XIANXIA_RPG_EFFECT_C4_LIGHT2:
    case H2_XIANXIA_RPG_EFFECT_C4_LIGHT3:
      game->flags |= 1u << (effect - H2_XIANXIA_RPG_EFFECT_C4_LIGHT1);
      if (effect == H2_XIANXIA_RPG_EFFECT_C4_LIGHT3)
        advance_stage(game, H2_XIANXIA_RPG_STAGE_C4_GUARD);
      rest_here(game);
      break;
    case H2_XIANXIA_RPG_EFFECT_C4_BATTLE:
      start_battle(game, H2_XIANXIA_RPG_ENEMY_INK_TIDE);
      break;
    case H2_XIANXIA_RPG_EFFECT_C4_HOME:
      game->flags |= H2_XIANXIA_RPG_FLAG_C4_FAINT;
      place_hero(game, H2_XIANXIA_RPG_MAP_NIGHT_VILLAGE, 7, 5);
      rest_here(game);
      particles_reset(game);
      break;
    case H2_XIANXIA_RPG_EFFECT_C4_REPORT:
      if ((game->flags & H2_XIANXIA_RPG_FLAG_C2_RUMOR) &&
          !(game->flags & H2_XIANXIA_RPG_FLAG_C4_BOOK)) {
        game->flags |= H2_XIANXIA_RPG_FLAG_C4_BOOK;
        start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C4_RUMOR);
      } else {
        game->flags |=
            H2_XIANXIA_RPG_FLAG_C4_BOOK | H2_XIANXIA_RPG_FLAG_C4_GLYPH;
        start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C4_DEPART);
      }
      break;
    case H2_XIANXIA_RPG_EFFECT_C3_BRIEF:
      if (game->stage == H2_XIANXIA_RPG_STAGE_C3_ASCEND)
        advance_stage(game, H2_XIANXIA_RPG_STAGE_C3_ARRAY);
      request_save(game);
      break;
    case H2_XIANXIA_RPG_EFFECT_C3_STELE1:
    case H2_XIANXIA_RPG_EFFECT_C3_STELE2:
    case H2_XIANXIA_RPG_EFFECT_C3_STELE3:
      game->flags |= H2_XIANXIA_RPG_FLAG_C3_STELE1
                     << (effect - H2_XIANXIA_RPG_EFFECT_C3_STELE1);
      request_save(game);
      break;
    case H2_XIANXIA_RPG_EFFECT_C3_CAMP:
      game->flags |= H2_XIANXIA_RPG_FLAG_C3_CAMP;
      restore(hero);
      game->rest_map = H2_XIANXIA_RPG_MAP_SWORD_HALL;
      game->rest_x = 3;
      game->rest_y = 7;
      request_save(game);
      break;
    case H2_XIANXIA_RPG_EFFECT_C3_BATTLE:
      start_battle(game, H2_XIANXIA_RPG_ENEMY_SWORD_SOUL);
      break;
    case H2_XIANXIA_RPG_EFFECT_C3_REVEAL:
      game->flags |=
          H2_XIANXIA_RPG_FLAG_C3_WENZHOU | H2_XIANXIA_RPG_FLAG_C3_GLYPH;
      request_save(game);
      start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C3_REPORT);
      break;
    case H2_XIANXIA_RPG_EFFECT_C2_BRIEFED:
      if (game->stage == H2_XIANXIA_RPG_STAGE_C2_ARRIVE)
        advance_stage(game, H2_XIANXIA_RPG_STAGE_C2_FIND_JIANGLI);
      request_save(game);
      break;
    case H2_XIANXIA_RPG_EFFECT_C2_TUTORIAL:
      start_fight(game, H2_XIANXIA_RPG_ENEMY_FOG_GHOST, true);
      break;
    case H2_XIANXIA_RPG_EFFECT_C2_JOINED:
      if (game->stage == H2_XIANXIA_RPG_STAGE_C2_FIND_JIANGLI)
        advance_stage(game, H2_XIANXIA_RPG_STAGE_C2_TO_REEDS);
      restore(hero);
      place_ally(game);
      request_save(game);
      break;
    case H2_XIANXIA_RPG_EFFECT_C2_TEA:
      game->flags |= H2_XIANXIA_RPG_FLAG_C2_RUMOR;
      rest_here(game);
      break;
    case H2_XIANXIA_RPG_EFFECT_C2_REEDS_TIP:
      game->flags |= H2_XIANXIA_RPG_FLAG_C2_REEDS_TIP;
      break;
    case H2_XIANXIA_RPG_EFFECT_C2_LIGHT_POST1:
      game->flags |= H2_XIANXIA_RPG_FLAG_C2_POST1;
      rest_here(game);
      break;
    case H2_XIANXIA_RPG_EFFECT_C2_LIGHT_POST2:
      game->flags |= H2_XIANXIA_RPG_FLAG_C2_POST2;
      rest_here(game);
      break;
    case H2_XIANXIA_RPG_EFFECT_C2_FISHER:
      game->flags |= H2_XIANXIA_RPG_FLAG_C2_FISHER;
      check_trust(game);
      break;
    case H2_XIANXIA_RPG_EFFECT_C2_ROPE:
      game->flags |= H2_XIANXIA_RPG_FLAG_C2_ROPE;
      check_trust(game);
      break;
    case H2_XIANXIA_RPG_EFFECT_C2_BASKET:
      if (!(game->flags & H2_XIANXIA_RPG_FLAG_C2_BASKET)) {
        game->flags |= H2_XIANXIA_RPG_FLAG_C2_BASKET;
        if (hero->pills < H2_XIANXIA_RPG_PILLS_MAX) ++hero->pills;
      }
      break;
    case H2_XIANXIA_RPG_EFFECT_C2_FOUND_BOAT:
      game->flags |= H2_XIANXIA_RPG_FLAG_C2_BOAT;
      if (game->stage == H2_XIANXIA_RPG_STAGE_C2_TO_REEDS)
        advance_stage(game, H2_XIANXIA_RPG_STAGE_C2_TO_WRECK);
      rest_here(game);
      break;
    case H2_XIANXIA_RPG_EFFECT_C2_SAIL:
      game->facing = H2_XIANXIA_RPG_FACING_RIGHT;
      place_hero(game, H2_XIANXIA_RPG_MAP_WRECK, 2, 5);
      particles_reset(game);
      if (game->stage < H2_XIANXIA_RPG_STAGE_C2_WRECK)
        advance_stage(game, H2_XIANXIA_RPG_STAGE_C2_WRECK);
      if (!(game->flags & H2_XIANXIA_RPG_FLAG_C2_POST4))
        start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C2_WRECK_LAND);
      break;
    case H2_XIANXIA_RPG_EFFECT_C2_LIGHT_POST4:
      /* The bow lamp post is the rest point before the boss. */
      game->flags |= H2_XIANXIA_RPG_FLAG_C2_POST4;
      restore(hero);
      game->rest_map = H2_XIANXIA_RPG_MAP_WRECK;
      game->rest_x = 5;
      game->rest_y = 3;
      request_save(game);
      break;
    case H2_XIANXIA_RPG_EFFECT_C2_GUARD_BATTLE:
      start_battle(game, H2_XIANXIA_RPG_ENEMY_WRECK_GUARD);
      break;
    case H2_XIANXIA_RPG_EFFECT_C2_WRAITH_BATTLE:
      start_battle(game, H2_XIANXIA_RPG_ENEMY_FOG_WRAITH);
      break;
    case H2_XIANXIA_RPG_EFFECT_C2_HOME:
      game->facing = H2_XIANXIA_RPG_FACING_UP;
      place_hero(game, H2_XIANXIA_RPG_MAP_DOCK, 9, 6);
      particles_reset(game);
      game->rest_map = H2_XIANXIA_RPG_MAP_DOCK;
      game->rest_x = 13;
      game->rest_y = 3;
      request_save(game);
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
  const char* page_end = d->script->lines[d->index].text + d->page_offset;
  for (uint32_t i = 0; i < d->total; ++i)
    (void)h2_xianxia_rpg_utf8_next(&page_end);
  if (*page_end != '\0') {
    d->page_offset = (uint32_t)(page_end - d->script->lines[d->index].text);
    d->revealed = d->reveal_ms = 0;
    d->total = dialogue_page_length(game);
    return;
  }
  if ((uint32_t)d->index + 1u < d->script->count) {
    ++d->index;
    d->page_offset = 0;
    d->revealed = 0;
    d->reveal_ms = 0;
    d->total = dialogue_page_length(game);
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

/* The thermal meter is chapter-local; all persistent narrative facts use
 * the separate word. Retreats and every ritual step commit a safe attempt. */
static void c6_meter(h2_xianxia_rpg_game_t* game, uint32_t meter) {
  game->flags = (game->flags & ~H2_XIANXIA_RPG_C6_METER_MASK) | meter;
}

static void c6_route(h2_xianxia_rpg_game_t* game, h2_xianxia_rpg_map_t map) {
  if (map != H2_XIANXIA_RPG_MAP_HERB_HUT &&
      !(game->facts & H2_XIANXIA_RPG_FACT_C6_CHOSEN)) {
    game->facts |= H2_XIANXIA_RPG_FACT_C6_CHOSEN;
    if (map == H2_XIANXIA_RPG_MAP_FROZEN_CREEK)
      game->facts |= H2_XIANXIA_RPG_FACT_C6_FIRST_COLD;
  }
  place_hero(game, map, map == H2_XIANXIA_RPG_MAP_HERB_HUT ? 7 : 1,
             map == H2_XIANXIA_RPG_MAP_HERB_HUT ? 5 : 4);
  c6_meter(game, 4);
  rest_here(game);
  start_dialogue(
      game, map == H2_XIANXIA_RPG_MAP_HOT_SPRING ? H2_XIANXIA_RPG_SCRIPT_C6_HOT
            : map == H2_XIANXIA_RPG_MAP_FROZEN_CREEK
                ? H2_XIANXIA_RPG_SCRIPT_C6_COLD
            : (game->facts & 3u) == 3u ? H2_XIANXIA_RPG_SCRIPT_C6_BOTH
                                       : H2_XIANXIA_RPG_SCRIPT_C6_REST);
}

int32_t h2_xianxia_rpg_c6_breath(const h2_xianxia_rpg_game_t* game) {
  static const int8_t breaths[] = {2, -2, 2, -2, -2, 2};
  return breaths[game->turns_taken % 6u];
}

static void c6_ritual_command(h2_xianxia_rpg_game_t* game, unsigned command) {
  int32_t meter = (int32_t)(game->flags & H2_XIANXIA_RPG_C6_METER_MASK);
  uint32_t streak = (game->flags >> H2_XIANXIA_RPG_C6_STREAK_SHIFT) & 7u;
  const bool wisp = game->turns_taken % 6u == 2u;
  const bool both = (game->facts & 3u) == 3u;
  if (command == 4u) {
    advance_stage(game, H2_XIANXIA_RPG_STAGE_C6_READY);
    game->flags = (game->flags & ~255u) | 4u;
    rest_here(game);
    start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C6_RETRY);
    return;
  }
  if (command != 3u) {
    if (!(command == 2u && wisp)) {
      meter += h2_xianxia_rpg_c6_breath(game);
      if (command == 0u) meter -= 2;
      if (command == 1u) meter += 2;
      if (wisp) meter += 2;
    }
    streak =
        meter >= (both ? 2 : 3) && meter <= (both ? 6 : 5) ? streak + 1u : 0u;
  }
  ++game->turns_taken;
  if (meter <= 0 || meter >= 8) {
    advance_stage(game, H2_XIANXIA_RPG_STAGE_C6_READY);
    game->flags = (game->flags & ~255u) | 4u;
    rest_here(game);
    start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C6_RETRY);
    return;
  }
  game->flags = (game->flags & ~255u) | (uint32_t)meter |
                (streak << H2_XIANXIA_RPG_C6_STREAK_SHIFT);
  if (streak >= 5u) {
    game->facts |= H2_XIANXIA_RPG_FACT_C6_HEALED;
    advance_stage(game, H2_XIANXIA_RPG_STAGE_C6_RECOVERED);
    restore_party(game);
    start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C6_RECOVERED);
  }
  request_save(game);
}

static void c9_route(h2_xianxia_rpg_game_t* game, h2_xianxia_rpg_map_t map) {
  const int32_t x = map == H2_XIANXIA_RPG_MAP_ECHO_HALL ? 2 : 7;
  const int32_t y = map == H2_XIANXIA_RPG_MAP_SHAFT_LOWER ? 2 : 5;
  place_hero(game, map, x, y);
  particles_reset(game);
  if (map == H2_XIANXIA_RPG_MAP_ECHO_HALL &&
      game->stage == H2_XIANXIA_RPG_STAGE_C9_DESCEND)
    advance_stage(game, H2_XIANXIA_RPG_STAGE_C9_ROOMS);
  rest_here(game);
  if (map == H2_XIANXIA_RPG_MAP_SHAFT_LOWER)
    start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C9_LOWER);
  if (map == H2_XIANXIA_RPG_MAP_ECHO_HALL)
    start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C9_HALL);
}
static void c9_interact(h2_xianxia_rpg_game_t* game, char tile) {
  if (tile == 'R') {
    rest_here(game);
    start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C9_REST);
  } else if (tile == 'I') {
    game->facts |= H2_XIANXIA_RPG_FACT_C9_BOOK;
    request_save(game);
    start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C9_BOOK);
  } else if (game->map == H2_XIANXIA_RPG_MAP_SHAFT_UPPER && tile == 'D') {
    if (!(game->facts & H2_XIANXIA_RPG_FACT_C9_BOOK))
      start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C9_NEED);
    else
      c9_route(game, H2_XIANXIA_RPG_MAP_SHAFT_LOWER);
  } else if (tile == 'U') {
    c9_route(game, H2_XIANXIA_RPG_MAP_SHAFT_UPPER);
  } else if (tile == 'E') {
    c9_route(game, game->map == H2_XIANXIA_RPG_MAP_ECHO_HALL
                       ? H2_XIANXIA_RPG_MAP_SHAFT_LOWER
                       : H2_XIANXIA_RPG_MAP_ECHO_HALL);
  } else if (game->map == H2_XIANXIA_RPG_MAP_ECHO_HALL && tile >= 'A' &&
             tile <= 'D') {
    game->node_room = (uint8_t)(tile - 'A');
    if (!(game->facts & H2_XIANXIA_RPG_FACT_C9_BOOK))
      start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C9_NEED);
    else if (game->flags & (1u << game->node_room))
      start_dialogue(
          game, (h2_xianxia_rpg_script_id_t)(H2_XIANXIA_RPG_SCRIPT_C9_MIST +
                                             game->node_room));
    else {
      game->glyph_view = 2u;
      game->glyph_wrong = false;
    }
  } else if (tile == 'F' && game->map == H2_XIANXIA_RPG_MAP_ECHO_HALL) {
    if ((game->flags & 240u) != 240u)
      start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C9_NEED);
    else
      c9_route(game, H2_XIANXIA_RPG_MAP_LANTERN_CORE);
  } else if (tile == 'F') {
    rest_here(game);
    start_dialogue(game, game->stage == H2_XIANXIA_RPG_STAGE_C9_FATHER
                             ? H2_XIANXIA_RPG_SCRIPT_C9_FATHER
                         : game->stage == H2_XIANXIA_RPG_STAGE_C9_REVEAL
                             ? H2_XIANXIA_RPG_SCRIPT_C9_REVEAL
                             : H2_XIANXIA_RPG_SCRIPT_C9_BATTLE);
  }
}
static void c9_glyph_tap(h2_xianxia_rpg_game_t* game, float x, float y) {
  if (inside(&game->layout.glyph_back, x, y)) {
    game->glyph_view = 0u;
    return;
  }
  if (game->glyph_view != 2u) return;
  for (uint32_t i = 0; i < 3u; ++i) {
    if (!inside(&game->layout.glyph_choices[i], x, y)) continue;
    if (!h2_xianxia_rpg_c9_wrong_glyph(game->node_room, i)) {
      game->glyph_wrong = true;
      return;
    }
    game->flags |= 1u << game->node_room;
    game->glyph_view = 0u;
    request_save(game);
    start_dialogue(game,
                   (h2_xianxia_rpg_script_id_t)(H2_XIANXIA_RPG_SCRIPT_C9_MIST +
                                                game->node_room));
    return;
  }
}

static void c8_route(h2_xianxia_rpg_game_t* game, h2_xianxia_rpg_map_t map) {
  const bool west = map == H2_XIANXIA_RPG_MAP_SNOW_CAVE;
  if (map != H2_XIANXIA_RPG_MAP_SNOW_CAMP) {
    if (!(game->facts & H2_XIANXIA_RPG_FACT_C8_CHOSEN)) {
      game->facts |= H2_XIANXIA_RPG_FACT_C8_CHOSEN;
      if (west) game->facts |= H2_XIANXIA_RPG_FACT_C8_WEST_FIRST;
      /* The second team has already weathered eight steps when reached. */
      game->flags = (game->flags & ~255u) | (west ? 8u : 128u);
      advance_stage(game, H2_XIANXIA_RPG_STAGE_C8_RESCUE);
    } else if (!(game->facts & H2_XIANXIA_RPG_C8_RESCUED) &&
               west !=
                   ((game->facts & H2_XIANXIA_RPG_FACT_C8_WEST_FIRST) != 0u)) {
      start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C8_NEED_RESCUE);
      return;
    }
  }
  place_hero(game, map, map == H2_XIANXIA_RPG_MAP_SNOW_CAMP ? 7 : 3,
             map == H2_XIANXIA_RPG_MAP_SNOW_CAMP ? 6 : 4);
  rest_here(game);
  start_dialogue(game, map == H2_XIANXIA_RPG_MAP_SNOW_CAMP
                           ? H2_XIANXIA_RPG_SCRIPT_C8_REST
                       : west ? H2_XIANXIA_RPG_SCRIPT_C8_WEST
                              : H2_XIANXIA_RPG_SCRIPT_C8_EAST);
}

static void c8_interact(h2_xianxia_rpg_game_t* game, char tile) {
  h2_xianxia_rpg_script_id_t script = H2_XIANXIA_RPG_SCRIPT_C8_HELP;
  const bool west = game->map == H2_XIANXIA_RPG_MAP_SNOW_CAVE;
  const uint32_t rescued =
      west ? H2_XIANXIA_RPG_FACT_C8_WEST : H2_XIANXIA_RPG_FACT_C8_EAST;
  if (tile == 'R') {
    rest_here(game);
    script = H2_XIANXIA_RPG_SCRIPT_C8_REST;
  } else if (tile == 'L') {
    game->facts |= H2_XIANXIA_RPG_FACT_C8_LIGHT;
    rest_here(game);
    script = H2_XIANXIA_RPG_SCRIPT_C8_LIGHT;
  } else if (tile == 'J' || tile == 'W') {
    if (tile == 'W' && !(game->facts & H2_XIANXIA_RPG_FACT_C8_WENZHOU)) {
      script = H2_XIANXIA_RPG_SCRIPT_C8_NEED_RESCUE;
    } else {
      game->facts &= ~H2_XIANXIA_RPG_FACT_C8_ACTIVE_WEN;
      if (tile == 'W') game->facts |= H2_XIANXIA_RPG_FACT_C8_ACTIVE_WEN;
      script = tile == 'W' ? H2_XIANXIA_RPG_SCRIPT_C8_WENZHOU
                           : H2_XIANXIA_RPG_SCRIPT_C8_JIANGLI;
      request_save(game);
    }
  } else if (tile == 'B') {
    if (game->stage == H2_XIANXIA_RPG_STAGE_C8_GUARD) {
      place_hero(game, H2_XIANXIA_RPG_MAP_SNOW_CAMP, 7, 6);
      rest_here(game);
      script = H2_XIANXIA_RPG_SCRIPT_C8_BATTLE;
    } else {
      script = game->stage == H2_XIANXIA_RPG_STAGE_C8_REVEAL
                   ? H2_XIANXIA_RPG_SCRIPT_C8_REVEAL
                   : H2_XIANXIA_RPG_SCRIPT_C8_NEED_RESCUE;
    }
  } else if (tile == 'T') {
    if (game->facts & rescued) {
      script = west ? ((game->flags & H2_XIANXIA_RPG_FLAG_C5_RETURN_LIGHT)
                           ? H2_XIANXIA_RPG_SCRIPT_C8_RECALL_GIVE
                           : H2_XIANXIA_RPG_SCRIPT_C8_RECALL_TAKE)
                    : H2_XIANXIA_RPG_SCRIPT_C8_REST;
    } else if (!(game->facts & H2_XIANXIA_RPG_C8_RESCUED)) {
      game->facts |= rescued | H2_XIANXIA_RPG_FACT_C8_COAL;
      if (west) game->facts |= H2_XIANXIA_RPG_FACT_C8_WENZHOU;
      const uint32_t pills = game->hero.pills + (west ? 1u : 2u);
      game->hero.pills =
          pills > H2_XIANXIA_RPG_PILLS_MAX ? H2_XIANXIA_RPG_PILLS_MAX : pills;
      rest_here(game);
      script = west ? H2_XIANXIA_RPG_SCRIPT_C8_FIRST_WEST
                    : H2_XIANXIA_RPG_SCRIPT_C8_FIRST_EAST;
    } else {
      advance_stage(game, H2_XIANXIA_RPG_STAGE_C8_INJURY);
      rest_here(game);
      script = H2_XIANXIA_RPG_SCRIPT_C8_INJURY;
    }
  }
  start_dialogue(game, script);
}

static void c8_action(h2_xianxia_rpg_game_t* game, unsigned action) {
  clear_path(game);
  if (game->stage == H2_XIANXIA_RPG_STAGE_C8_INJURY) {
    if (action == 1u && game->hero.pills == 0u) {
      start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C8_NO_PILL);
      return;
    }
    if (action == 0u)
      game->facts &= ~H2_XIANXIA_RPG_FACT_C8_COAL;
    else {
      --game->hero.pills;
      game->facts |= H2_XIANXIA_RPG_FACT_C8_PILL_USED;
    }
    game->facts |= H2_XIANXIA_RPG_C8_RESCUED | H2_XIANXIA_RPG_FACT_C8_WENZHOU;
    advance_stage(game, H2_XIANXIA_RPG_STAGE_C8_GUARD);
    rest_here(game);
    start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C8_RESCUED);
  } else if (game->map == H2_XIANXIA_RPG_MAP_SNOW_CAMP) {
    c8_route(game, action == 0u ? H2_XIANXIA_RPG_MAP_ICE_CLIFF
                                : H2_XIANXIA_RPG_MAP_SNOW_CAVE);
  } else if (action == 0u) {
    c8_route(game, H2_XIANXIA_RPG_MAP_SNOW_CAMP);
  } else {
    start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C8_HELP);
  }
}

static void c7_interact(h2_xianxia_rpg_game_t* game, char tile, int32_t x) {
  const bool past = (game->flags & H2_XIANXIA_RPG_FLAG_C7_PAST) != 0u;
  h2_xianxia_rpg_script_id_t script = H2_XIANXIA_RPG_SCRIPT_C7_HELP;
  if (tile == 'M') {
    game->flags ^= H2_XIANXIA_RPG_FLAG_C7_PAST;
    place_hero(game, game->map, x, 4);
    rest_here(game);
    script =
        past ? H2_XIANXIA_RPG_SCRIPT_C7_NOW : H2_XIANXIA_RPG_SCRIPT_C7_PAST;
  } else if (tile == 'L') {
    if (!past)
      script = H2_XIANXIA_RPG_SCRIPT_C7_NEED_PAST;
    else {
      game->facts |= H2_XIANXIA_RPG_FACT_C7_LAMP;
      if (game->stage == H2_XIANXIA_RPG_STAGE_C7_LAMP)
        advance_stage(game, H2_XIANXIA_RPG_STAGE_C7_TREATY);
      script = H2_XIANXIA_RPG_SCRIPT_C7_LAMP;
      request_save(game);
    }
  } else if (tile == 'W') {
    if (past)
      script = H2_XIANXIA_RPG_SCRIPT_C7_NEED_NOW;
    else {
      game->facts |= H2_XIANXIA_RPG_FACT_C7_WATER;
      script = H2_XIANXIA_RPG_SCRIPT_C7_WATER;
      request_save(game);
    }
  } else if (tile == 'A') {
    if (!past)
      script = H2_XIANXIA_RPG_SCRIPT_C7_NEED_PAST;
    else if (!(game->facts & H2_XIANXIA_RPG_FACT_C7_WATER))
      script = H2_XIANXIA_RPG_SCRIPT_C7_NEED_TREATY;
    else {
      game->facts |= H2_XIANXIA_RPG_FACT_C7_TREATY;
      if (game->stage == H2_XIANXIA_RPG_STAGE_C7_TREATY)
        advance_stage(game, H2_XIANXIA_RPG_STAGE_C7_QUESTION1);
      script = H2_XIANXIA_RPG_SCRIPT_C7_TREATY;
      request_save(game);
    }
  } else if (tile == 'E') {
    if (game->map == H2_XIANXIA_RPG_MAP_MIRROR_HALL) {
      place_hero(game, H2_XIANXIA_RPG_MAP_MIRROR_SHORE, 11, 4);
      rest_here(game);
    } else if (past)
      script = H2_XIANXIA_RPG_SCRIPT_C7_NEED_NOW;
    else if (!(game->facts & H2_XIANXIA_RPG_FACT_C7_LAMP))
      script = H2_XIANXIA_RPG_SCRIPT_C7_NEED_LAMP;
    else {
      place_hero(game, H2_XIANXIA_RPG_MAP_MIRROR_HALL, 3, 4);
      rest_here(game);
    }
  } else if (tile == 'H') {
    script = game->stage == H2_XIANXIA_RPG_STAGE_C7_DUEL
                 ? H2_XIANXIA_RPG_SCRIPT_C7_BATTLE
             : game->stage == H2_XIANXIA_RPG_STAGE_C7_REVEAL
                 ? H2_XIANXIA_RPG_SCRIPT_C7_REVEAL
                 : H2_XIANXIA_RPG_SCRIPT_C7_NEED_TREATY;
  }
  start_dialogue(game, script);
}

static void interact(h2_xianxia_rpg_game_t* game, int32_t x, int32_t y) {
  if (game->chapter == 9u) {
    c9_interact(game, h2_xianxia_rpg_tile(game->map, x, y));
    return;
  }
  if (game->chapter == 8u) {
    c8_interact(game, h2_xianxia_rpg_tile(game->map, x, y));
    return;
  }
  if (game->chapter == 7u) {
    c7_interact(game,
                game->map == H2_XIANXIA_RPG_MAP_MIRROR_SHORE &&
                        (game->flags & H2_XIANXIA_RPG_FLAG_C7_PAST) &&
                        x == 10 && y == 3
                    ? 'L'
                    : h2_xianxia_rpg_tile(game->map, x, y),
                x);
    return;
  }
  game->facing = facing_toward(x - game->tx, y - game->ty);
  h2_xianxia_rpg_npc_t npc;
  if (npc_at(game, x, y, &npc)) {
    const h2_xianxia_rpg_script_id_t speech =
        h2_xianxia_rpg_talk_script(game, (h2_xianxia_rpg_actor_t)npc.actor);
    if (game->chapter == 5u && speech == H2_XIANXIA_RPG_SCRIPT_C5_PICKUP) {
      advance_stage(game, H2_XIANXIA_RPG_STAGE_C5_ESCORT);
      game->flags = (game->flags & ~H2_XIANXIA_RPG_C5_STEPS_MASK) |
                    (14u << H2_XIANXIA_RPG_C5_STEPS_SHIFT);
      request_save(game);
    }
    start_dialogue(game, speech);
    return;
  }
  const char tile = h2_xianxia_rpg_tile(game->map, x, y);
  if (game->map == H2_XIANXIA_RPG_MAP_SWORD_COURT && tile >= '1' &&
      tile <= '3') {
    if (!(game->flags & H2_XIANXIA_RPG_FLAG_C3_OPEN)) {
      const uint32_t post = (uint32_t)(tile - '1'), shift = 2u * post;
      const uint32_t direction =
          (h2_xianxia_rpg_c3_post_direction(game->flags, post) + 1u) % 4u;
      game->flags = (game->flags & ~(3u << shift)) | direction << shift;
      if (h2_xianxia_rpg_c3_posts_aligned(game->flags)) {
        game->flags |= H2_XIANXIA_RPG_FLAG_C3_OPEN;
        advance_stage(game, H2_XIANXIA_RPG_STAGE_C3_HALL);
        start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C3_OPEN);
      }
      request_save(game);
    } else {
      start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C3_CENTER);
    }
    return;
  }
  h2_xianxia_rpg_script_id_t script = h2_xianxia_rpg_tile_script(game, x, y);
  if (game->chapter == 6u) {
    if (tile == 'S' || tile == 'F') {
      c6_meter(game, 4);
      rest_here(game);
    } else if (tile == 'm') {
      const uint32_t meter = game->flags & H2_XIANXIA_RPG_C6_METER_MASK;
      if (meter < 2u || meter > 6u) {
        start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C6_NEED_BALANCE);
        return;
      }
      game->facts |= game->map == H2_XIANXIA_RPG_MAP_HOT_SPRING
                         ? H2_XIANXIA_RPG_FACT_C6_HOT
                         : H2_XIANXIA_RPG_FACT_C6_COLD;
      if (game->stage == H2_XIANXIA_RPG_STAGE_C6_HERBS)
        advance_stage(game, H2_XIANXIA_RPG_STAGE_C6_READY);
      request_save(game);
    }
  }
  if (game->chapter == 5u) {
    if (tile == 'B') {
      game->flags ^= H2_XIANXIA_RPG_FLAG_C5_HIGH;
      /* The bell always returns the party to its permanent landing. */
      place_hero(game, game->map,
                 game->map == H2_XIANXIA_RPG_MAP_OLD_CITY ? 3 : 4, 4);
      rest_here(game);
      particles_reset(game);
    } else if (tile == 'p') {
      game->flags |= H2_XIANXIA_RPG_FLAG_C5_PAGE;
      if (game->stage == H2_XIANXIA_RPG_STAGE_C5_PAGE)
        advance_stage(game, H2_XIANXIA_RPG_STAGE_C5_TOWER);
      request_save(game);
    }
  }
  /* The visible fire/lamp and its checkpoint change on the player's tap,
   * before the accompanying news says it is lit and saved. */
  if (game->chapter == 4u) {
    if (script == H2_XIANXIA_RPG_SCRIPT_C4_FIRE ||
        script == H2_XIANXIA_RPG_SCRIPT_C4_REST)
      apply_effect(game, H2_XIANXIA_RPG_EFFECT_C4_FIRE);
    else if (script >= H2_XIANXIA_RPG_SCRIPT_C4_LAMP1 &&
             script <= H2_XIANXIA_RPG_SCRIPT_C4_LAMP3)
      apply_effect(
          game,
          (h2_xianxia_rpg_effect_t)(H2_XIANXIA_RPG_EFFECT_C4_LIGHT1 + script -
                                    H2_XIANXIA_RPG_SCRIPT_C4_LAMP1));
  }
  start_dialogue(game, script);
}

static void arrive(h2_xianxia_rpg_game_t* game) {
  if (game->chapter == 8u && game->map != H2_XIANXIA_RPG_MAP_SNOW_CAMP) {
    const bool west = game->map == H2_XIANXIA_RPG_MAP_SNOW_CAVE;
    const uint32_t shift = west ? 4u : 0u;
    const uint32_t steps = (game->flags >> shift) & 15u;
    if (steps < 12u) {
      game->flags = (game->flags & ~(15u << shift)) | ((steps + 1u) << shift);
      if (steps == 11u) {
        game->facts |= west ? H2_XIANXIA_RPG_FACT_C8_WEST_BLOCK
                            : H2_XIANXIA_RPG_FACT_C8_EAST_BLOCK;
        /* Closing underfoot moves to the adjacent permanent landing. */
        if (h2_xianxia_rpg_tile(game->map, game->tx, game->ty) == 's')
          place_hero(game, game->map,
                     game->facing == H2_XIANXIA_RPG_FACING_RIGHT ? 8 : 6, 4);
        clear_path(game);
        start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C8_BLOCK);
      }
      request_save(game);
    }
    return;
  }
  if (game->chapter == 6u) {
    int32_t meter = (int32_t)(game->flags & H2_XIANXIA_RPG_C6_METER_MASK);
    const bool slide_end = game->map == H2_XIANXIA_RPG_MAP_FROZEN_CREEK &&
                           game->path_pos == game->path_len;
    if (game->map == H2_XIANXIA_RPG_MAP_HOT_SPRING &&
        h2_xianxia_rpg_tile(game->map, game->tx, game->ty) == 'h')
      meter += 2;
    if (slide_end) --meter;
    if (meter <= 0 || meter >= 8) {
      place_hero(game, game->rest_map, game->rest_x, game->rest_y);
      c6_meter(game, 4);
      rest_here(game);
      start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C6_RETREAT);
      return;
    }
    c6_meter(game, (uint32_t)meter);
    if (slide_end) request_save(game);
  }
  if (game->chapter == 5u && game->stage == H2_XIANXIA_RPG_STAGE_C5_ESCORT &&
      !(game->flags & H2_XIANXIA_RPG_FLAG_C5_HIGH)) {
    uint32_t remaining = (game->flags & H2_XIANXIA_RPG_C5_STEPS_MASK) >>
                         H2_XIANXIA_RPG_C5_STEPS_SHIFT;
    if (remaining > 0u) --remaining;
    game->flags = (game->flags & ~H2_XIANXIA_RPG_C5_STEPS_MASK) |
                  (remaining << H2_XIANXIA_RPG_C5_STEPS_SHIFT);
    if (remaining == 0u) {
      advance_stage(game, H2_XIANXIA_RPG_STAGE_C5_RESCUE);
      game->flags |= H2_XIANXIA_RPG_FLAG_C5_HIGH;
      place_hero(game, H2_XIANXIA_RPG_MAP_OLD_CITY, 3, 4);
      rest_here(game);
      start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C5_RETRY);
      return;
    }
  }
  if (game->chapter == 4u && game->stage < H2_XIANXIA_RPG_STAGE_C4_RETURN &&
      (game->flags & H2_XIANXIA_RPG_FLAG_C4_FIRE) &&
      h2_xianxia_rpg_tile(game->map, game->tx, game->ty) == 'w') {
    clear_path(game);
    if (game->flags & H2_XIANXIA_RPG_FLAG_C4_COVER) {
      start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C4_SAFE);
    } else {
      place_hero(game, game->rest_map, game->rest_x, game->rest_y);
      restore_party(game);
      particles_reset(game);
      start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C4_OUT);
    }
    return;
  }
  h2_xianxia_rpg_map_t to_map;
  int32_t to_x, to_y;
  if (h2_xianxia_rpg_exit_at(game->map, game->tx, game->ty, &to_map, &to_x,
                             &to_y)) {
    const h2_xianxia_rpg_script_id_t closed =
        h2_xianxia_rpg_exit_closed(game, game->map);
    if (closed != H2_XIANXIA_RPG_SCRIPT_COUNT) {
      clear_path(game);
      start_dialogue(game, closed);
      return;
    }
    if (game->chapter == 6u) {
      c6_route(game, to_map);
      return;
    }
    place_hero(game, to_map, to_x, to_y);
    particles_reset(game);
    if (game->chapter == 5u && game->stage == H2_XIANXIA_RPG_STAGE_C5_ESCORT &&
        to_map == H2_XIANXIA_RPG_MAP_WATER_MARKET) {
      advance_stage(game, H2_XIANXIA_RPG_STAGE_C5_RETURN);
      game->flags = (game->flags | H2_XIANXIA_RPG_FLAG_C5_SAVED) &
                    ~H2_XIANXIA_RPG_C5_STEPS_MASK;
      rest_here(game);
      start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C5_SAFE);
      return;
    }
    start_dialogue(game, h2_xianxia_rpg_enter_script(game, to_map));
    return;
  }
  h2_xianxia_rpg_enemy_t enemies[H2_XIANXIA_RPG_ENCOUNTER_MAX];
  const size_t count = h2_xianxia_rpg_encounters(game->map, enemies);
  if (count == 0u ||
      !h2_xianxia_rpg_ambush_cell(game, game->map, game->tx, game->ty))
    return;
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
    game->ally_from_x = game->ally_x;
    game->ally_from_y = game->ally_y;
    arrive(game);
    /* A snow-step checkpoint must be stored before the next animation
     * advances tx/ty; the App consumes writes once per frame (<=50 ms). */
    if (game->chapter == 8u) return;
    if (game->phase != H2_XIANXIA_RPG_PHASE_EXPLORE || game->dialogue.active)
      return;
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
    if (game->ally_visible) {
      /* The companion steps into the tile the hero leaves. */
      game->ally_from_x = game->ally_x;
      game->ally_from_y = game->ally_y;
      game->ally_x = game->tx;
      game->ally_y = game->ty;
    }
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
  if (game->chapter == 9u) {
    if (game->glyph_view) {
      c9_glyph_tap(game, x, y);
      return;
    }
    if (game->moving || game->path_pos < game->path_len) return;
    if (inside(&l->escort_actions[0], x, y)) {
      if (game->facts & H2_XIANXIA_RPG_FACT_C9_BOOK)
        game->glyph_view = 1u;
      else
        start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C9_NEED);
      return;
    }
    if (inside(&l->escort_actions[1], x, y)) {
      start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C9_HELP);
      return;
    }
    const h2_xianxia_rpg_rect_t ally =
        rect(l->map_x + game->ally_x * l->tile,
             l->map_y + game->ally_y * l->tile, l->tile, l->tile);
    if (inside(&ally, x, y)) {
      game->facts ^= H2_XIANXIA_RPG_FACT_C9_ACTIVE_WEN;
      request_save(game);
      return;
    }
  }
  if (game->chapter == 8u) {
    if (game->moving || game->path_pos < game->path_len) return;
    for (unsigned i = 0; i < 2u; ++i) {
      if (!inside(&l->escort_actions[i], x, y)) continue;
      c8_action(game, i);
      return;
    }
  }
  if (game->chapter == 7u) {
    if (game->moving || game->path_pos < game->path_len) return;
    const bool question = game->stage >= H2_XIANXIA_RPG_STAGE_C7_QUESTION1 &&
                          game->stage <= H2_XIANXIA_RPG_STAGE_C7_QUESTION3;
    for (unsigned i = 0; i < 2u; ++i) {
      if (!inside(&l->escort_actions[i], x, y)) continue;
      if (question) {
        const uint32_t q =
            (uint32_t)(game->stage - H2_XIANXIA_RPG_STAGE_C7_QUESTION1);
        if (i) {
          game->facts |= 1u << (H2_XIANXIA_RPG_C7_ANSWERS_SHIFT + q);
          if (game->trust < H2_XIANXIA_RPG_TRUST_MAX) ++game->trust;
        } else if (game->trust)
          --game->trust;
        advance_stage(game, (h2_xianxia_rpg_stage_t)(game->stage + 1));
        game->flags |= H2_XIANXIA_RPG_FLAG_C7_FEEDBACK;
        request_save(game);
        start_dialogue(game, h2_xianxia_rpg_c7_feedback(game));
      } else {
        start_dialogue(game, i == 0u ? H2_XIANXIA_RPG_SCRIPT_C7_HELP
                             : game->stage >= H2_XIANXIA_RPG_STAGE_C7_DUEL
                                 ? h2_xianxia_rpg_c7_feedback(game)
                             : game->flags & H2_XIANXIA_RPG_FLAG_C5_RETURN_LIGHT
                                 ? H2_XIANXIA_RPG_SCRIPT_C7_RECALL_GIVE
                                 : H2_XIANXIA_RPG_SCRIPT_C7_RECALL_TAKE);
      }
      return;
    }
    if (question) return;
  }
  if (game->chapter == 6u) {
    if (game->moving || game->path_pos < game->path_len) return;
    if (game->stage == H2_XIANXIA_RPG_STAGE_C6_RITUAL) {
      for (unsigned i = 0; i < H2_XIANXIA_RPG_COMMANDS; ++i)
        if (inside(&l->commands[i], x, y)) c6_ritual_command(game, i);
      return;
    }
    if (inside(&l->escort_actions[0], x, y) ||
        inside(&l->escort_actions[1], x, y)) {
      if (game->stage == H2_XIANXIA_RPG_STAGE_C6_ARRIVE) {
        start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C6_HELP);
      } else if (game->map == H2_XIANXIA_RPG_MAP_HERB_HUT) {
        c6_route(game, inside(&l->escort_actions[0], x, y)
                           ? H2_XIANXIA_RPG_MAP_HOT_SPRING
                           : H2_XIANXIA_RPG_MAP_FROZEN_CREEK);
      } else if (inside(&l->escort_actions[0], x, y)) {
        c6_route(game, H2_XIANXIA_RPG_MAP_HERB_HUT);
      } else {
        start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C6_HELP);
      }
      return;
    }
    if (game->map == H2_XIANXIA_RPG_MAP_FROZEN_CREEK) {
      const int32_t col = (int32_t)((x - (float)l->map_x) / (float)l->tile);
      const int32_t row = (int32_t)((y - (float)l->map_y) / (float)l->tile);
      if (x < (float)l->map_x || y < (float)l->map_y ||
          col >= H2_XIANXIA_RPG_MAP_COLS || row >= H2_XIANXIA_RPG_MAP_ROWS)
        return;
      const int32_t dx = col == game->tx ? 0 : col > game->tx ? 1 : -1;
      const int32_t dy = row == game->ty ? 0 : row > game->ty ? 1 : -1;
      if ((dx != 0 && dy != 0) || (dx == 0 && dy == 0)) return;
      if (interactive(game, col, row) &&
          abs(col - game->tx) + abs(row - game->ty) == 1) {
        interact(game, col, row);
        return;
      }
      clear_path(game);
      int32_t nx = game->tx + dx, ny = game->ty + dy;
      while (passable(game, nx, ny) &&
             game->path_len < H2_XIANXIA_RPG_PATH_MAX) {
        game->path_x[game->path_len] = (uint8_t)nx;
        game->path_y[game->path_len++] = (uint8_t)ny;
        if (h2_xianxia_rpg_tile(game->map, nx, ny) == '=') break;
        nx += dx;
        ny += dy;
      }
      return;
    }
  }
  if (game->chapter == 5u && !game->moving) {
    if (game->stage == H2_XIANXIA_RPG_STAGE_C5_CHOICE) {
      for (int i = 0; i < 2; ++i) {
        if (!inside(&l->escort_actions[i], x, y)) continue;
        if (i)
          game->flags |= H2_XIANXIA_RPG_FLAG_C5_RETURN_LIGHT;
        else
          game->flags &= ~H2_XIANXIA_RPG_FLAG_C5_RETURN_LIGHT;
        /* Commit the choice and checkpoint before showing the evidence. */
        advance_stage(game, H2_XIANXIA_RPG_STAGE_C5_DUEL);
        rest_here(game);
        start_dialogue(game, i ? H2_XIANXIA_RPG_SCRIPT_C5_RETURN_LIGHT
                               : H2_XIANXIA_RPG_SCRIPT_C5_TAKE);
      }
      return;
    }
    if (inside(&l->escort_actions[1], x, y)) {
      clear_path(game);
      start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C5_HELP);
      return;
    }
    if (inside(&l->escort_actions[0], x, y) &&
        game->map == H2_XIANXIA_RPG_MAP_OLD_CITY &&
        game->stage < H2_XIANXIA_RPG_STAGE_C5_RESCUE) {
      start_battle(game, H2_XIANXIA_RPG_ENEMY_TIDE_OTTER);
      return;
    }
  }
  if (game->chapter == 4u && !game->moving) {
    if (inside(&l->escort_actions[0], x, y)) {
      clear_path(game);
      game->flags ^= H2_XIANXIA_RPG_FLAG_C4_COVER;
      start_dialogue(game, game->flags & H2_XIANXIA_RPG_FLAG_C4_COVER
                               ? H2_XIANXIA_RPG_SCRIPT_C4_COVER
                               : H2_XIANXIA_RPG_SCRIPT_C4_SCOUT);
      request_save(game);
      return;
    }
    if (inside(&l->escort_actions[1], x, y)) {
      clear_path(game);
      start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C4_HELP);
      return;
    }
  }
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

/* ---- Title, chapters and endings --------------------------------------- */

static void new_game(h2_xianxia_rpg_game_t* game) {
  h2_xianxia_rpg_hero_at_level(&game->hero, 1u);
  game->chapter = 1u;
  game->stage = H2_XIANXIA_RPG_STAGE_MEET_ELDER;
  game->objective_ms = 0;
  game->flags = 0;
  game->facts = 0;
  game->trust = 0;
  game->battles_won = game->turns_taken = 0;
  game->facing = H2_XIANXIA_RPG_FACING_UP;
  place_hero(game, H2_XIANXIA_RPG_MAP_VILLAGE, 7, 5);
  game->rest_map = H2_XIANXIA_RPG_MAP_VILLAGE;
  game->rest_x = 8;
  game->rest_y = 5;
  enter_phase(game, H2_XIANXIA_RPG_PHASE_EXPLORE);
  start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_INTRO);
}

/* Begin a later chapter with the hero of the previous chapter's clear, or
 * with the chapter's defaults when there is none. The opening is saved. */
static void start_chapter(h2_xianxia_rpg_game_t* game, uint32_t chapter,
                          const h2_xianxia_rpg_clear_t* carry) {
  const h2_xianxia_rpg_chapter_info_t* info =
      h2_xianxia_rpg_chapter_info(chapter);
  if (info == NULL || chapter == 1u) {
    new_game(game);
    return;
  }
  if (carry != NULL) {
    h2_xianxia_rpg_hero_at_level(&game->hero, carry->level);
    if (carry->exp > game->hero.exp) game->hero.exp = carry->exp;
    game->hero.pills = carry->pills <= H2_XIANXIA_RPG_PILLS_MAX
                           ? carry->pills
                           : H2_XIANXIA_RPG_PILLS_MAX;
    game->flags = (carry->flags & H2_XIANXIA_RPG_FLAG_CARRY) |
                  (chapter == 2u ? H2_XIANXIA_RPG_FLAG_C1_CLEARED : 0u);
    game->trust = carry->trust <= H2_XIANXIA_RPG_TRUST_MAX
                      ? carry->trust
                      : H2_XIANXIA_RPG_TRUST_MAX;
  } else {
    h2_xianxia_rpg_hero_at_level(&game->hero, info->default_level);
    game->hero.pills = info->default_pills;
    game->flags = 0;
    game->trust = 0;
  }
  game->facts = 0u;
  game->glyph_view = 0u;
  if (chapter == 9u) {
    game->facts = H2_XIANXIA_RPG_FACT_C6_HEALED |
                  H2_XIANXIA_RPG_FACT_C7_MIRROR | H2_XIANXIA_RPG_C8_RESCUED |
                  H2_XIANXIA_RPG_FACT_C8_CHOSEN |
                  H2_XIANXIA_RPG_FACT_C8_WENZHOU | H2_XIANXIA_RPG_FACT_C8_SNOW;
    if (carry != NULL) {
      const uint32_t c8 = game->records.progress.reserved[7];
      game->facts =
          (game->records.progress.reserved[5] & 31u) |
          ((uint32_t)(game->records.progress.reserved[6] & 15u) << 8) |
          ((c8 & 1u) << 12) | ((c8 & 6u) << 15) | ((c8 & 8u) << 20) |
          ((c8 & 16u) << 14);
      if (game->records.has_journey8 &&
          game->records.journey8.stage == H2_XIANXIA_RPG_STAGE_C8_CLEARED)
        game->facts = game->records.journey8.facts;
    }
    if (game->facts & H2_XIANXIA_RPG_FACT_C8_ACTIVE_WEN)
      game->facts |= H2_XIANXIA_RPG_FACT_C9_ACTIVE_WEN;
    game->flags &= ~255u;
  }
  if (chapter == 8u) {
    game->facts = H2_XIANXIA_RPG_FACT_C6_HEALED | H2_XIANXIA_RPG_FACT_C7_MIRROR;
    if (carry != NULL) {
      game->facts = (game->records.progress.reserved[5] & 31u) |
                    ((uint32_t)(game->records.progress.reserved[6] & 15u) << 8);
      if (game->records.has_journey7 &&
          game->records.journey7.stage == H2_XIANXIA_RPG_STAGE_C7_CLEARED)
        game->facts = game->records.journey7.facts;
    }
    game->flags &= ~255u;
  }
  if (chapter == 7u) {
    game->facts = carry != NULL ? game->records.progress.reserved[5] & 31u
                                : H2_XIANXIA_RPG_FACT_C6_HEALED;
    game->flags &= ~255u;
  }
  if (chapter == 6u) game->flags = (game->flags & ~255u) | 4u;
  if (chapter == 3u)
    game->flags = (game->flags & ~H2_XIANXIA_RPG_C3_POST_BITS) |
                  H2_XIANXIA_RPG_C3_POST_INITIAL;
  if (chapter == 5u)
    game->flags |= H2_XIANXIA_RPG_FLAG_C4_JOINED | H2_XIANXIA_RPG_FLAG_C4_BOOK;
  game->chapter = chapter;
  restore_party(game);
  game->stage = info->first_stage;
  game->objective_ms = 0;
  game->battles_won = game->turns_taken = 0;
  game->facing = H2_XIANXIA_RPG_FACING_DOWN;
  place_hero(game, info->start_map, info->start_x, info->start_y);
  game->rest_map = info->rest_map;
  game->rest_x = info->rest_x;
  game->rest_y = info->rest_y;
  enter_phase(game, H2_XIANXIA_RPG_PHASE_EXPLORE);
  start_dialogue(game, (h2_xianxia_rpg_script_id_t)info->intro_script);
  request_save(game);
}

static bool cell_usable(h2_xianxia_rpg_game_t* game, uint32_t map, uint32_t x,
                        uint32_t y) {
  if (h2_xianxia_rpg_map_chapter((h2_xianxia_rpg_map_t)map) != game->chapter)
    return false;
  const h2_xianxia_rpg_map_t saved = game->map;
  game->map = (h2_xianxia_rpg_map_t)map;
  const bool usable =
      h2_xianxia_rpg_walkable(game, game->map, (int32_t)x, (int32_t)y) &&
      !npc_at(game, (int32_t)x, (int32_t)y, NULL);
  game->map = saved;
  return usable;
}

static void continue_journey(h2_xianxia_rpg_game_t* game,
                             const h2_xianxia_rpg_save_t* s) {
  const h2_xianxia_rpg_chapter_info_t* info =
      h2_xianxia_rpg_chapter_info(s->chapter);
  if (info == NULL) return;
  h2_xianxia_rpg_hero_at_level(&game->hero, s->level);
  if (s->exp > game->hero.exp) game->hero.exp = s->exp;
  game->hero.pills = s->pills;
  game->chapter = s->chapter;
  game->glyph_view = 0u;
  restore_party(game);
  game->stage = (h2_xianxia_rpg_stage_t)s->stage;
  game->flags = s->flags;
  game->facts = s->facts;
  game->trust = s->trust;
  game->battles_won = s->battles;
  game->turns_taken = s->turns;
  game->objective_ms = 0;
  game->facing = H2_XIANXIA_RPG_FACING_DOWN;
  h2_xianxia_rpg_map_t map = (h2_xianxia_rpg_map_t)s->map;
  int32_t x = (int32_t)s->x, y = (int32_t)s->y;
  if (!cell_usable(game, s->map, s->x, s->y)) {
    map = info->rest_map;
    x = info->rest_x;
    y = info->rest_y;
  }
  if (game->chapter == 5u &&
      (game->stage == H2_XIANXIA_RPG_STAGE_C5_RESCUE ||
       game->stage == H2_XIANXIA_RPG_STAGE_C5_ESCORT) &&
      map != H2_XIANXIA_RPG_MAP_OLD_CITY) {
    map = H2_XIANXIA_RPG_MAP_OLD_CITY;
    x = 3;
    y = 4;
  }
  place_hero(game, map, x, y);
  if (cell_usable(game, s->rest_map, s->rest_x, s->rest_y)) {
    game->rest_map = (h2_xianxia_rpg_map_t)s->rest_map;
    game->rest_x = (int32_t)s->rest_x;
    game->rest_y = (int32_t)s->rest_y;
  } else {
    game->rest_map = map;
    game->rest_x = x;
    game->rest_y = y;
  }
  /* A finished chapter reopens its result rather than the boss fight. */
  enter_phase(game, game->stage == info->cleared_stage
                        ? H2_XIANXIA_RPG_PHASE_ENDING
                        : H2_XIANXIA_RPG_PHASE_EXPLORE);
  if (game->stage == H2_XIANXIA_RPG_STAGE_C9_REVEAL)
    start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C9_REVEAL);
  if (game->stage == H2_XIANXIA_RPG_STAGE_C8_REVEAL)
    start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C8_REVEAL);
  if (game->stage == H2_XIANXIA_RPG_STAGE_C8_INJURY)
    start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C8_INJURY);
  if (game->chapter == 7u && (game->flags & H2_XIANXIA_RPG_FLAG_C7_FEEDBACK))
    start_dialogue(game, h2_xianxia_rpg_c7_feedback(game));
  else if (game->stage == H2_XIANXIA_RPG_STAGE_C7_REVEAL)
    start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C7_REVEAL);
  if (game->stage == H2_XIANXIA_RPG_STAGE_C6_RECOVERED)
    start_dialogue(game, H2_XIANXIA_RPG_SCRIPT_C6_RECOVERED);
}

/* The journey Continue resumes: the latest saved one. */
static const h2_xianxia_rpg_save_t* continue_target(
    const h2_xianxia_rpg_game_t* game) {
  const h2_xianxia_rpg_records_t* r = &game->records;
  if (r->has_journey9 &&
      (r->journey9_latest ||
       (!r->has_journey8 && !r->has_journey7 && !r->has_journey6 &&
        !r->has_journey5 && !r->has_journey4 && !r->has_journey3 &&
        !r->has_journey && !r->has_chapter1)))
    return &r->journey9;
  if (r->has_journey8 &&
      (r->journey8_latest ||
       (!r->has_journey7 && !r->has_journey6 && !r->has_journey5 &&
        !r->has_journey4 && !r->has_journey3 && !r->has_journey &&
        !r->has_chapter1)))
    return &r->journey8;
  if (r->has_journey7 &&
      (r->journey7_latest ||
       (!r->has_journey6 && !r->has_journey5 && !r->has_journey4 &&
        !r->has_journey3 && !r->has_journey && !r->has_chapter1)))
    return &r->journey7;
  if (r->has_journey6 &&
      (r->journey6_latest ||
       (!r->has_journey5 && !r->has_journey4 && !r->has_journey3 &&
        !r->has_journey && !r->has_chapter1)))
    return &r->journey6;
  if (r->has_journey5 &&
      (r->journey5_latest || (!r->has_journey4 && !r->has_journey3 &&
                              !r->has_journey && !r->has_chapter1)))
    return &r->journey5;
  if (r->has_journey4 &&
      (r->journey4_latest ||
       (!r->has_journey3 && !r->has_journey && !r->has_chapter1)))
    return &r->journey4;
  if (r->has_journey3 &&
      (r->journey3_latest || (!r->has_journey && !r->has_chapter1)))
    return &r->journey3;
  if (r->has_journey && (r->journey_latest || !r->has_chapter1))
    return &r->journey;
  return r->has_chapter1 ? &r->chapter1 : NULL;
}

const h2_xianxia_rpg_save_t* h2_xianxia_rpg_continue_save(
    const h2_xianxia_rpg_game_t* game) {
  return continue_target(game);
}

const h2_xianxia_rpg_save_t* h2_xianxia_rpg_unfinished(
    const h2_xianxia_rpg_game_t* game, uint32_t chapter) {
  const h2_xianxia_rpg_records_t* r = &game->records;
  const h2_xianxia_rpg_chapter_info_t* info =
      h2_xianxia_rpg_chapter_info(chapter);
  if (info == NULL) return NULL;
  const h2_xianxia_rpg_save_t* s =
      chapter == 9u   ? (r->has_journey9 ? &r->journey9 : NULL)
      : chapter == 8u ? (r->has_journey8 ? &r->journey8 : NULL)
      : chapter == 7u ? (r->has_journey7 ? &r->journey7 : NULL)
      : chapter == 6u ? (r->has_journey6 ? &r->journey6 : NULL)
      : chapter == 5u ? (r->has_journey5 ? &r->journey5 : NULL)
      : chapter == 4u ? (r->has_journey4 ? &r->journey4 : NULL)
      : chapter == 3u ? (r->has_journey3 ? &r->journey3 : NULL)
      : chapter == 1u ? (r->has_chapter1 ? &r->chapter1 : NULL)
                      : (r->has_journey ? &r->journey : NULL);
  if (s == NULL || s->chapter != chapter ||
      s->stage >= (uint32_t)info->cleared_stage)
    return NULL;
  return s;
}

static void enter_prologue(h2_xianxia_rpg_game_t* game, uint32_t chapter) {
  game->prologue_chapter = chapter;
  game->prologue_carry = chapter >= 2u && (game->records.progress.cleared &
                                           (1u << (chapter - 2u))) != 0u;
  enter_phase(game, H2_XIANXIA_RPG_PHASE_PROLOGUE);
}

static void prologue_tap(h2_xianxia_rpg_game_t* game) {
  if (game->time_ms < game->phase_started_ms + PROLOGUE_TAP_MS) return;
  const uint32_t chapter = game->prologue_chapter;
  start_chapter(game, chapter,
                game->prologue_carry
                    ? &game->records.progress.chapters[chapter - 2u]
                    : NULL);
}

static void title_tap(h2_xianxia_rpg_game_t* game, float x, float y) {
  const h2_xianxia_rpg_save_t* resume = continue_target(game);
  if (inside(&game->layout.title_buttons[0], x, y))
    new_game(game);
  else if (resume != NULL && inside(&game->layout.title_buttons[1], x, y))
    continue_journey(game, resume);
  else if (inside(&game->layout.title_buttons[2], x, y))
    enter_phase(game, H2_XIANXIA_RPG_PHASE_CHAPTERS);
}

static void chapters_tap(h2_xianxia_rpg_game_t* game, float x, float y) {
  const h2_xianxia_rpg_layout_t* l = &game->layout;
  if (inside(&l->chapter_back, x, y)) {
    enter_phase(game, H2_XIANXIA_RPG_PHASE_TITLE);
    return;
  }
  if (game->height < 400 && inside(&l->chapter_more, x, y)) {
    game->chapter_page ^= 1u;
    return;
  }
  for (uint32_t i = 0; i < H2_XIANXIA_RPG_CHAPTERS_PLAYABLE; ++i) {
    if (game->height < 400 && i / 5u != game->chapter_page) continue;
    if (!inside(&l->chapter_rows[i], x, y)) continue;
    const uint32_t chapter = i + 1u;
    const h2_xianxia_rpg_save_t* unfinished =
        h2_xianxia_rpg_unfinished(game, chapter);
    const bool restart =
        unfinished != NULL && inside(&l->chapter_restart[i], x, y);
    if (unfinished != NULL && !restart) {
      const h2_xianxia_rpg_save_t copy = *unfinished;
      continue_journey(game, &copy);
    } else if (chapter == 1u) {
      new_game(game);
    } else {
      enter_prologue(game, chapter);
    }
    return;
  }
}

static void ending_tap(h2_xianxia_rpg_game_t* game) {
  if (game->time_ms < game->phase_started_ms + ENDING_TAP_MS) return;
  /* Automatic onward never replaces an existing or unknown journey9. */
  if (game->chapter == 8u && game->records.has_journey9) {
    const h2_xianxia_rpg_save_t saved = game->records.journey9;
    continue_journey(game, &saved);
    return;
  }
  if (game->chapter == 8u && game->records.journey9_unreadable) {
    enter_phase(game, H2_XIANXIA_RPG_PHASE_TITLE);
    return;
  }
  /* Automatic onward never replaces an existing or unknown journey8. */
  if (game->chapter == 7u && game->records.has_journey8) {
    const h2_xianxia_rpg_save_t saved = game->records.journey8;
    continue_journey(game, &saved);
    return;
  }
  if (game->chapter == 7u && game->records.journey8_unreadable) {
    enter_phase(game, H2_XIANXIA_RPG_PHASE_TITLE);
    return;
  }
  /* Automatic onward never replaces an existing or unknown journey7. */
  if (game->chapter == 6u && game->records.has_journey7) {
    const h2_xianxia_rpg_save_t saved = game->records.journey7;
    continue_journey(game, &saved);
    return;
  }
  if (game->chapter == 6u && game->records.journey7_unreadable) {
    enter_phase(game, H2_XIANXIA_RPG_PHASE_TITLE);
    return;
  }
  /* Each released chapter flows into the next playable one. */
  if (h2_xianxia_rpg_chapter_info(game->chapter + 1u) != NULL)
    enter_prologue(game, game->chapter + 1u);
  else
    enter_phase(game, H2_XIANXIA_RPG_PHASE_TITLE);
}

/* ---- Public entry points ---------------------------------------------- */

static h2_xianxia_rpg_clear_t clear_of(const h2_xianxia_rpg_save_t* s) {
  return (h2_xianxia_rpg_clear_t){.level = s->level,
                                  .exp = s->exp,
                                  .pills = s->pills,
                                  .battles = s->battles,
                                  .turns = s->turns,
                                  .flags = s->flags,
                                  .trust = s->trust};
}

void h2_xianxia_rpg_game_init(h2_xianxia_rpg_game_t* game, int32_t width,
                              int32_t height, uint32_t seed,
                              const h2_xianxia_rpg_records_t* records) {
  memset(game, 0, sizeof(*game));
  game->width = width;
  game->height = height;
  const int32_t short_side = width < height ? width : height;
  game->unit = (float)short_side / 480.0f;
  game->rng = seed != 0u ? seed : 0x5EEDu;
  game->target_x = game->target_y = -1;
  game->chapter = 1u;
  compute_layout(game);
  if (records != NULL) {
    h2_xianxia_rpg_records_t* r = &game->records;
    *r = *records;
    /* Keep only parts that pass the same checks as stored data. */
    if (r->has_chapter1) {
      uint32_t words[H2_XIANXIA_RPG_SAVE_WORDS];
      h2_xianxia_rpg_save_pack(&records->chapter1, words);
      r->has_chapter1 = h2_xianxia_rpg_save_unpack(words, &r->chapter1);
    }
    if (r->has_journey) {
      uint8_t bytes[H2_XIANXIA_RPG_JOURNEY_BYTES];
      uint32_t tag = 0;
      h2_xianxia_rpg_journey_encode(&records->journey, 0u, bytes);
      r->has_journey = h2_xianxia_rpg_journey_decode(bytes, sizeof(bytes),
                                                     &r->journey, &tag);
    }
    if (r->has_journey3) {
      uint8_t bytes[H2_XIANXIA_RPG_JOURNEY_BYTES];
      h2_xianxia_rpg_journey_encode(&records->journey3, 0u, bytes);
      r->has_journey3 = h2_xianxia_rpg_journey_decode(bytes, sizeof(bytes),
                                                      &r->journey3, NULL) &&
                        r->journey3.chapter == 3u;
    }
    if (r->has_journey4) {
      uint8_t bytes[H2_XIANXIA_RPG_JOURNEY_BYTES];
      h2_xianxia_rpg_journey_encode(&records->journey4, 0u, bytes);
      r->has_journey4 = h2_xianxia_rpg_journey_decode(bytes, sizeof(bytes),
                                                      &r->journey4, NULL) &&
                        r->journey4.chapter == 4u;
    }
    if (r->has_journey5) {
      uint8_t bytes[H2_XIANXIA_RPG_JOURNEY_BYTES];
      h2_xianxia_rpg_journey_encode(&records->journey5, 0u, bytes);
      r->has_journey5 = h2_xianxia_rpg_journey_decode(bytes, sizeof(bytes),
                                                      &r->journey5, NULL) &&
                        r->journey5.chapter == 5u;
    }
    if (r->has_journey6) {
      uint8_t bytes[H2_XIANXIA_RPG_JOURNEY6_BYTES];
      h2_xianxia_rpg_journey6_encode(&records->journey6, 0u, bytes);
      r->has_journey6 = h2_xianxia_rpg_journey6_decode(bytes, sizeof(bytes),
                                                       &r->journey6, NULL);
    }
    if (r->has_journey7) {
      uint8_t bytes[H2_XIANXIA_RPG_JOURNEY7_BYTES];
      h2_xianxia_rpg_journey7_encode(&records->journey7, 0u, bytes);
      r->has_journey7 = h2_xianxia_rpg_journey7_decode(bytes, sizeof(bytes),
                                                       &r->journey7, NULL);
    }
    if (r->has_journey8) {
      uint8_t bytes[H2_XIANXIA_RPG_JOURNEY8_BYTES];
      h2_xianxia_rpg_journey8_encode(&records->journey8, 0u, bytes);
      r->has_journey8 = h2_xianxia_rpg_journey8_decode(bytes, sizeof(bytes),
                                                       &r->journey8, NULL);
    }
    if (r->has_journey9) {
      uint8_t bytes[H2_XIANXIA_RPG_JOURNEY9_BYTES];
      h2_xianxia_rpg_journey9_encode(&records->journey9, 0u, bytes);
      r->has_journey9 = h2_xianxia_rpg_journey9_decode(bytes, sizeof(bytes),
                                                       &r->journey9, NULL);
    }
    if (!r->has_journey9) r->journey9_latest = false;
    /* The ninth entry can inherit a real eighth result even if the shared
     * completion table is missing or opaque. Keep this fallback in memory;
     * never repair that table on load. */
    if (r->has_journey8 &&
        r->journey8.stage == H2_XIANXIA_RPG_STAGE_C8_CLEARED &&
        !(r->progress.cleared & 128u)) {
      r->progress.cleared |= 128u;
      r->progress.chapters[7] = clear_of(&r->journey8);
    }
    if (!r->has_journey8) r->journey8_latest = false;
    if (!r->has_journey7) r->journey7_latest = false;
    if (!r->has_journey6) r->journey6_latest = false;
    if (!r->has_journey5) r->journey5_latest = false;
    if (!r->has_journey4) r->journey4_latest = false;
    if (!r->has_journey3) r->journey3_latest = false;
    if (!r->has_journey) r->journey_latest = false;
    /* A v0.1 clear lives only in save0..save3: copy it into the chapter
     * record so a new chapter-1 run cannot erase it. */
    if (r->has_chapter1 && r->chapter1.stage == H2_XIANXIA_RPG_STAGE_CLEARED) {
      const h2_xianxia_rpg_clear_t clear = clear_of(&r->chapter1);
      if (!(r->progress.cleared & 1u)) {
        r->progress.cleared |= 1u;
        r->progress.chapters[0] = clear;
        /* Keep the usable legacy clear in memory even if chapters cannot
         * be read, but never replace that blob just by opening the game.
         * A player completing a chapter still explicitly records a clear. */
        game->progress_pending = !r->progress_unreadable;
      }
    }
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
      ending_tap(game);
      break;
    case H2_XIANXIA_RPG_PHASE_CHAPTERS:
      chapters_tap(game, x, y);
      break;
    case H2_XIANXIA_RPG_PHASE_PROLOGUE:
      prologue_tap(game);
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

bool h2_xianxia_rpg_game_take_writes(h2_xianxia_rpg_game_t* game,
                                     h2_xianxia_rpg_writes_t* out) {
  if (!game->save_pending && !game->progress_pending) return false;
  h2_xianxia_rpg_writes_t writes;
  memset(&writes, 0, sizeof(writes));
  h2_xianxia_rpg_records_t* r = &game->records;
  if (game->save_pending) {
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
        .chapter = game->chapter,
        .trust = game->trust,
        .rest_map = (uint32_t)game->rest_map,
        .rest_x = (uint32_t)game->rest_x,
        .rest_y = (uint32_t)game->rest_y,
        .facts = game->facts,
    };
    if (game->chapter < 9u) r->journey9_latest = false;
    if (game->chapter < 8u) r->journey8_latest = false;
    if (game->chapter < 7u) r->journey7_latest = false;
    if (game->chapter < 6u) r->journey6_latest = false;
    if (game->chapter < 5u) r->journey5_latest = false;
    if (game->chapter < 4u) r->journey4_latest = false;
    if (game->chapter == 9u) {
      writes.journey9 = true;
      writes.journey9_save = save;
      r->has_journey9 = r->journey9_latest = true;
      r->journey9 = save;
    } else if (game->chapter == 8u) {
      writes.journey8 = true;
      writes.journey8_save = save;
      r->has_journey8 = r->journey8_latest = true;
      r->journey8 = save;
    } else if (game->chapter == 7u) {
      writes.journey7 = true;
      writes.journey7_save = save;
      r->has_journey7 = r->journey7_latest = true;
      r->journey7 = save;
    } else if (game->chapter == 6u) {
      writes.journey6 = true;
      writes.journey6_save = save;
      r->has_journey6 = r->journey6_latest = true;
      r->journey6 = save;
    } else if (game->chapter == 5u) {
      writes.journey5 = true;
      writes.journey5_save = save;
      r->has_journey5 = r->journey5_latest = true;
      r->journey5 = save;
    } else if (game->chapter == 4u) {
      writes.journey4 = true;
      writes.journey4_save = save;
      r->has_journey4 = r->journey4_latest = true;
      r->journey4 = save;
    } else if (game->chapter == 3u) {
      writes.journey3 = true;
      writes.journey3_save = save;
      r->has_journey3 = r->journey3_latest = true;
      r->journey3 = save;
    } else if (game->chapter == 1u) {
      writes.chapter1 = true;
      writes.chapter1_save = save;
      r->has_chapter1 = true;
      r->chapter1 = save;
      r->journey_latest = false;
      r->journey3_latest = false;
    } else {
      writes.journey = true;
      writes.journey_save = save;
      r->has_journey = true;
      r->journey = save;
      r->journey_latest = true;
      r->journey3_latest = false;
    }
  }
  if (game->progress_pending) {
    writes.progress = true;
    writes.progress_record = r->progress;
  }
  game->save_pending = game->progress_pending = false;
  if (out != NULL) *out = writes;
  return true;
}

/* ---- Stored formats ---------------------------------------------------- */

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
  h2_xianxia_rpg_save_t save = {
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
      .chapter = 1u,
  };
  /* The v0.1 rules: chapter-1 stages and maps, and its one flag. */
  if (save.stage > (uint32_t)H2_XIANXIA_RPG_STAGE_CLEARED || save.level < 1u ||
      save.level > H2_XIANXIA_RPG_LEVEL_MAX ||
      save.pills > H2_XIANXIA_RPG_PILLS_MAX ||
      h2_xianxia_rpg_map_chapter((h2_xianxia_rpg_map_t)save.map) != 1u ||
      save.x >= (uint32_t)H2_XIANXIA_RPG_MAP_COLS ||
      save.y >= (uint32_t)H2_XIANXIA_RPG_MAP_ROWS ||
      (save.flags & ~H2_XIANXIA_RPG_FLAG_PILLS_GIVEN) != 0u)
    return false;
  save.rest_map = save.map;
  save.rest_x = save.x;
  save.rest_y = save.y;
  *out_save = save;
  return true;
}

static uint32_t fnv1a(uint32_t seed, const uint8_t* data, size_t size) {
  uint32_t hash = 2166136261u ^ seed;
  for (size_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= 16777619u;
  }
  return hash;
}

uint32_t h2_xianxia_rpg_predecessor_tag(uint32_t chapter1_tag,
                                        const uint8_t* chapter2, size_t size) {
  return chapter1_tag ^ fnv1a(RECORD_SEED_JOURNEY, chapter2, size);
}

static void put_u16(uint8_t* out, uint32_t value) {
  out[0] = (uint8_t)(value & 0xFFu);
  out[1] = (uint8_t)(value >> 8 & 0xFFu);
}

static void put_u32(uint8_t* out, uint32_t value) {
  for (int i = 0; i < 4; ++i) out[i] = (uint8_t)(value >> (8 * i) & 0xFFu);
}

static uint32_t get_u16(const uint8_t* in) {
  return (uint32_t)in[0] | (uint32_t)in[1] << 8;
}

static uint32_t get_u32(const uint8_t* in) {
  return (uint32_t)in[0] | (uint32_t)in[1] << 8 | (uint32_t)in[2] << 16 |
         (uint32_t)in[3] << 24;
}

static uint32_t clamp_u16(uint32_t value) {
  return value < 0xFFFFu ? value : 0xFFFFu;
}

uint32_t h2_xianxia_rpg_chapter1_tag(
    const uint32_t words[H2_XIANXIA_RPG_SAVE_WORDS], unsigned present_mask) {
  if ((present_mask & 0xFu) == 0u) return 0u;
  uint8_t bytes[4u * H2_XIANXIA_RPG_SAVE_WORDS + 1u];
  for (size_t i = 0; i < H2_XIANXIA_RPG_SAVE_WORDS; ++i)
    put_u32(bytes + 4u * i, (present_mask >> i & 1u) ? words[i] : 0u);
  bytes[sizeof(bytes) - 1u] = (uint8_t)(present_mask & 0xFu);
  const uint32_t tag = fnv1a(RECORD_SEED_CHAPTER1, bytes, sizeof(bytes));
  return tag != 0u ? tag : 1u;
}

/* Journey blob, little endian: version, chapter, stage, level, pills, map,
 * x, y, rest map, rest x, rest y, trust, exp u32, flags u32, battles u16,
 * turns u16, chapter-1 tag u32, checksum u32 over the preceding bytes. */
void h2_xianxia_rpg_journey_encode(const h2_xianxia_rpg_save_t* save,
                                   uint32_t chapter1_tag,
                                   uint8_t out[H2_XIANXIA_RPG_JOURNEY_BYTES]) {
  memset(out, 0, H2_XIANXIA_RPG_JOURNEY_BYTES);
  out[0] = (uint8_t)H2_XIANXIA_RPG_RECORD_VERSION;
  out[1] = (uint8_t)(save->chapter & 0xFFu);
  out[2] = (uint8_t)(save->stage & 0xFFu);
  out[3] = (uint8_t)(save->level & 0xFFu);
  out[4] = (uint8_t)(save->pills & 0xFFu);
  out[5] = (uint8_t)(save->map & 0xFFu);
  out[6] = (uint8_t)(save->x & 0xFFu);
  out[7] = (uint8_t)(save->y & 0xFFu);
  out[8] = (uint8_t)(save->rest_map & 0xFFu);
  out[9] = (uint8_t)(save->rest_x & 0xFFu);
  out[10] = (uint8_t)(save->rest_y & 0xFFu);
  out[11] = (uint8_t)(save->trust & 0xFFu);
  put_u32(out + 12, save->exp);
  put_u32(out + 16, save->flags);
  put_u16(out + 20, clamp_u16(save->battles));
  put_u16(out + 22, clamp_u16(save->turns));
  put_u32(out + 24, chapter1_tag);
  put_u32(out + 28, fnv1a(RECORD_SEED_JOURNEY, out, 28u));
}

static bool chapter_map(const h2_xianxia_rpg_chapter_info_t* info,
                        uint32_t map) {
  return map >= (uint32_t)info->first_map &&
         map < (uint32_t)info->first_map + info->map_count;
}

bool h2_xianxia_rpg_journey_decode(const uint8_t* data, size_t size,
                                   h2_xianxia_rpg_save_t* out_save,
                                   uint32_t* out_chapter1_tag) {
  if (data == NULL || size != H2_XIANXIA_RPG_JOURNEY_BYTES ||
      data[0] != (uint8_t)H2_XIANXIA_RPG_RECORD_VERSION ||
      get_u32(data + 28) != fnv1a(RECORD_SEED_JOURNEY, data, 28u))
    return false;
  const h2_xianxia_rpg_save_t save = {
      .chapter = data[1],
      .stage = data[2],
      .level = data[3],
      .pills = data[4],
      .map = data[5],
      .x = data[6],
      .y = data[7],
      .rest_map = data[8],
      .rest_x = data[9],
      .rest_y = data[10],
      .trust = data[11],
      .exp = get_u32(data + 12),
      .flags = get_u32(data + 16),
      .battles = get_u16(data + 20),
      .turns = get_u16(data + 22),
  };
  const h2_xianxia_rpg_chapter_info_t* info =
      save.chapter >= 2u ? h2_xianxia_rpg_chapter_info(save.chapter) : NULL;
  if (info == NULL || save.stage < (uint32_t)info->first_stage ||
      save.stage > (uint32_t)info->cleared_stage || save.level < 1u ||
      save.level > H2_XIANXIA_RPG_LEVEL_MAX ||
      save.pills > H2_XIANXIA_RPG_PILLS_MAX ||
      save.trust > H2_XIANXIA_RPG_TRUST_MAX || !chapter_map(info, save.map) ||
      !chapter_map(info, save.rest_map) ||
      save.x >= (uint32_t)H2_XIANXIA_RPG_MAP_COLS ||
      save.y >= (uint32_t)H2_XIANXIA_RPG_MAP_ROWS ||
      save.rest_x >= (uint32_t)H2_XIANXIA_RPG_MAP_COLS ||
      save.rest_y >= (uint32_t)H2_XIANXIA_RPG_MAP_ROWS)
    return false;
  *out_save = save;
  if (out_chapter1_tag != NULL) *out_chapter1_tag = get_u32(data + 24);
  return true;
}

void h2_xianxia_rpg_journey6_encode(
    const h2_xianxia_rpg_save_t* save, uint32_t predecessor_tag,
    uint8_t out[H2_XIANXIA_RPG_JOURNEY6_BYTES]) {
  uint8_t legacy[H2_XIANXIA_RPG_JOURNEY_BYTES];
  h2_xianxia_rpg_journey_encode(save, predecessor_tag, legacy);
  memcpy(out, legacy, 28u);
  out[0] = 3u;
  put_u32(out + 28, save->facts);
  put_u32(out + 32, fnv1a(RECORD_SEED_JOURNEY, out, 32u));
}

bool h2_xianxia_rpg_journey6_decode(const uint8_t* data, size_t size,
                                    h2_xianxia_rpg_save_t* out_save,
                                    uint32_t* out_tag) {
  if (data == NULL || size != H2_XIANXIA_RPG_JOURNEY6_BYTES || data[0] != 3u ||
      data[1] != 6u ||
      get_u32(data + 32) != fnv1a(RECORD_SEED_JOURNEY, data, 32u))
    return false;
  uint8_t legacy[H2_XIANXIA_RPG_JOURNEY_BYTES];
  memcpy(legacy, data, 28u);
  legacy[0] = (uint8_t)H2_XIANXIA_RPG_RECORD_VERSION;
  put_u32(legacy + 28, fnv1a(RECORD_SEED_JOURNEY, legacy, 28u));
  h2_xianxia_rpg_save_t save;
  if (!h2_xianxia_rpg_journey_decode(legacy, sizeof(legacy), &save, out_tag))
    return false;
  save.facts = get_u32(data + 28);
  /* Unknown fact bits remain opaque instead of being erased by a checkpoint. */
  if ((save.facts & ~31u) != 0u || (save.flags & 15u) > 8u ||
      ((save.flags >> 4) & 15u) > 5u)
    return false;
  if (save.stage >= H2_XIANXIA_RPG_STAGE_C6_READY && !(save.facts & 3u))
    return false;
  if (save.stage >= H2_XIANXIA_RPG_STAGE_C6_RECOVERED &&
      !(save.facts & H2_XIANXIA_RPG_FACT_C6_HEALED))
    return false;
  *out_save = save;
  return true;
}

void h2_xianxia_rpg_journey7_encode(
    const h2_xianxia_rpg_save_t* save, uint32_t predecessor_tag,
    uint8_t out[H2_XIANXIA_RPG_JOURNEY7_BYTES]) {
  h2_xianxia_rpg_journey6_encode(save, predecessor_tag, out);
}

bool h2_xianxia_rpg_journey7_decode(const uint8_t* data, size_t size,
                                    h2_xianxia_rpg_save_t* out_save,
                                    uint32_t* out_tag) {
  if (data == NULL || size != H2_XIANXIA_RPG_JOURNEY7_BYTES || data[0] != 3u ||
      data[1] != 7u ||
      get_u32(data + 32) != fnv1a(RECORD_SEED_JOURNEY, data, 32u))
    return false;
  uint8_t legacy[H2_XIANXIA_RPG_JOURNEY_BYTES];
  memcpy(legacy, data, 28u);
  legacy[0] = (uint8_t)H2_XIANXIA_RPG_RECORD_VERSION;
  put_u32(legacy + 28, fnv1a(RECORD_SEED_JOURNEY, legacy, 28u));
  h2_xianxia_rpg_save_t save;
  if (!h2_xianxia_rpg_journey_decode(legacy, sizeof(legacy), &save, out_tag))
    return false;
  save.facts = get_u32(data + 28);
  if ((save.facts & ~4095u) != 0u || (save.flags & 252u) != 0u) return false;
  if (save.stage >= H2_XIANXIA_RPG_STAGE_C7_TREATY &&
      !(save.facts & H2_XIANXIA_RPG_FACT_C7_LAMP))
    return false;
  if (save.stage >= H2_XIANXIA_RPG_STAGE_C7_QUESTION1 &&
      (save.facts &
       (H2_XIANXIA_RPG_FACT_C7_WATER | H2_XIANXIA_RPG_FACT_C7_TREATY)) !=
          (H2_XIANXIA_RPG_FACT_C7_WATER | H2_XIANXIA_RPG_FACT_C7_TREATY))
    return false;
  if (save.stage >= H2_XIANXIA_RPG_STAGE_C7_REVEAL &&
      !(save.facts & H2_XIANXIA_RPG_FACT_C7_MIRROR))
    return false;
  if ((save.flags & H2_XIANXIA_RPG_FLAG_C7_FEEDBACK) &&
      (save.stage < H2_XIANXIA_RPG_STAGE_C7_QUESTION2 ||
       save.stage > H2_XIANXIA_RPG_STAGE_C7_DUEL))
    return false;
  *out_save = save;
  return true;
}

void h2_xianxia_rpg_journey8_encode(
    const h2_xianxia_rpg_save_t* save, uint32_t predecessor_tag,
    uint8_t out[H2_XIANXIA_RPG_JOURNEY8_BYTES]) {
  h2_xianxia_rpg_journey6_encode(save, predecessor_tag, out);
}

bool h2_xianxia_rpg_journey8_decode(const uint8_t* data, size_t size,
                                    h2_xianxia_rpg_save_t* out_save,
                                    uint32_t* out_tag) {
  if (data == NULL || size != H2_XIANXIA_RPG_JOURNEY8_BYTES || data[0] != 3u ||
      data[1] != 8u ||
      get_u32(data + 32) != fnv1a(RECORD_SEED_JOURNEY, data, 32u))
    return false;
  uint8_t legacy[H2_XIANXIA_RPG_JOURNEY_BYTES];
  memcpy(legacy, data, 28u);
  legacy[0] = (uint8_t)H2_XIANXIA_RPG_RECORD_VERSION;
  put_u32(legacy + 28, fnv1a(RECORD_SEED_JOURNEY, legacy, 28u));
  h2_xianxia_rpg_save_t save;
  if (!h2_xianxia_rpg_journey_decode(legacy, sizeof(legacy), &save, out_tag))
    return false;
  save.facts = get_u32(data + 28);
  if ((save.facts & ~0xFFFFFFu) != 0u || (save.flags & 15u) > 12u ||
      ((save.flags >> 4) & 15u) > 12u)
    return false;
  if (save.stage >= H2_XIANXIA_RPG_STAGE_C8_RESCUE &&
      !(save.facts & H2_XIANXIA_RPG_FACT_C8_CHOSEN))
    return false;
  if (save.stage == H2_XIANXIA_RPG_STAGE_C8_INJURY &&
      (!(save.facts & H2_XIANXIA_RPG_FACT_C8_COAL) ||
       (save.facts & H2_XIANXIA_RPG_C8_RESCUED) == 0u ||
       (save.facts & H2_XIANXIA_RPG_C8_RESCUED) == H2_XIANXIA_RPG_C8_RESCUED))
    return false;
  if (save.stage >= H2_XIANXIA_RPG_STAGE_C8_GUARD &&
      (save.facts & H2_XIANXIA_RPG_C8_RESCUED) != H2_XIANXIA_RPG_C8_RESCUED)
    return false;
  if ((save.facts & H2_XIANXIA_RPG_FACT_C8_ACTIVE_WEN) &&
      !(save.facts & H2_XIANXIA_RPG_FACT_C8_WENZHOU))
    return false;
  if (save.stage >= H2_XIANXIA_RPG_STAGE_C8_REVEAL &&
      !(save.facts & H2_XIANXIA_RPG_FACT_C8_SNOW))
    return false;
  *out_save = save;
  return true;
}

void h2_xianxia_rpg_journey9_encode(
    const h2_xianxia_rpg_save_t* save, uint32_t tag,
    uint8_t out[H2_XIANXIA_RPG_JOURNEY9_BYTES]) {
  h2_xianxia_rpg_journey6_encode(save, tag, out);
}
bool h2_xianxia_rpg_journey9_decode(const uint8_t* data, size_t size,
                                    h2_xianxia_rpg_save_t* out_save,
                                    uint32_t* out_tag) {
  if (data == NULL || size != H2_XIANXIA_RPG_JOURNEY9_BYTES || data[0] != 3u ||
      data[1] != 9u ||
      get_u32(data + 32) != fnv1a(RECORD_SEED_JOURNEY, data, 32u))
    return false;
  uint8_t legacy[H2_XIANXIA_RPG_JOURNEY_BYTES];
  memcpy(legacy, data, 28u);
  legacy[0] = (uint8_t)H2_XIANXIA_RPG_RECORD_VERSION;
  put_u32(legacy + 28, fnv1a(RECORD_SEED_JOURNEY, legacy, 28u));
  h2_xianxia_rpg_save_t save;
  if (!h2_xianxia_rpg_journey_decode(legacy, sizeof(legacy), &save, out_tag))
    return false;
  save.facts = get_u32(data + 28);
  if (save.facts & ~0xFFFFFFFu) return false;
  /* Completed trials imply opened doors; later stages imply all four. */
  if (((save.flags >> 4) & 15u) & ~(save.flags & 15u)) return false;
  if (save.stage >= H2_XIANXIA_RPG_STAGE_C9_ROOMS &&
      !(save.facts & H2_XIANXIA_RPG_FACT_C9_BOOK))
    return false;
  if (save.stage >= H2_XIANXIA_RPG_STAGE_C9_FATHER &&
      (save.flags & 255u) != 255u)
    return false;
  if (save.stage >= H2_XIANXIA_RPG_STAGE_C9_BATTLE &&
      !(save.facts & H2_XIANXIA_RPG_FACT_C9_FATHER))
    return false;
  if (save.stage >= H2_XIANXIA_RPG_STAGE_C9_REVEAL &&
      !(save.facts & H2_XIANXIA_RPG_FACT_C9_CLEAR))
    return false;
  *out_save = save;
  return true;
}

/* Chapter record: version, chapter slots, cleared bits u16, then per
 * chapter level, pills, trust, zero, exp u32, flags u32, battles u16,
 * turns u16, and a checksum u32 over the preceding bytes. */
void h2_xianxia_rpg_progress_encode(
    const h2_xianxia_rpg_progress_t* progress,
    uint8_t out[H2_XIANXIA_RPG_PROGRESS_BYTES]) {
  memset(out, 0, H2_XIANXIA_RPG_PROGRESS_BYTES);
  out[0] = (uint8_t)H2_XIANXIA_RPG_RECORD_VERSION;
  out[1] = (uint8_t)H2_XIANXIA_RPG_CHAPTER_MAX;
  put_u16(out + 2,
          progress->cleared & ((1u << H2_XIANXIA_RPG_CHAPTER_MAX) - 1u));
  for (uint32_t c = 0; c < H2_XIANXIA_RPG_CHAPTER_MAX; ++c) {
    const h2_xianxia_rpg_clear_t* clear = &progress->chapters[c];
    uint8_t* at = out + 4u + c * H2_XIANXIA_RPG_CLEAR_BYTES;
    at[0] = (uint8_t)(clear->level & 0xFFu);
    at[1] = (uint8_t)(clear->pills & 0xFFu);
    at[2] = (uint8_t)(clear->trust & 0xFFu);
    at[3] = progress->reserved[c];
    put_u32(at + 4, clear->exp);
    put_u32(at + 8, clear->flags);
    put_u16(at + 12, clamp_u16(clear->battles));
    put_u16(at + 14, clamp_u16(clear->turns));
  }
  const size_t body = H2_XIANXIA_RPG_PROGRESS_BYTES - 4u;
  put_u32(out + body, fnv1a(RECORD_SEED_PROGRESS, out, body));
}

bool h2_xianxia_rpg_progress_decode(const uint8_t* data, size_t size,
                                    h2_xianxia_rpg_progress_t* out) {
  const size_t body = H2_XIANXIA_RPG_PROGRESS_BYTES - 4u;
  if (data == NULL || size != H2_XIANXIA_RPG_PROGRESS_BYTES ||
      data[0] != (uint8_t)H2_XIANXIA_RPG_RECORD_VERSION ||
      data[1] != (uint8_t)H2_XIANXIA_RPG_CHAPTER_MAX ||
      get_u32(data + body) != fnv1a(RECORD_SEED_PROGRESS, data, body))
    return false;
  h2_xianxia_rpg_progress_t progress;
  memset(&progress, 0, sizeof(progress));
  progress.cleared = get_u16(data + 2);
  if (progress.cleared >> H2_XIANXIA_RPG_CHAPTER_MAX) return false;
  for (uint32_t c = 0; c < H2_XIANXIA_RPG_CHAPTER_MAX; ++c) {
    const uint8_t* at = data + 4u + c * H2_XIANXIA_RPG_CLEAR_BYTES;
    h2_xianxia_rpg_clear_t* clear = &progress.chapters[c];
    progress.reserved[c] = at[3];
    *clear = (h2_xianxia_rpg_clear_t){.level = at[0],
                                      .pills = at[1],
                                      .trust = at[2],
                                      .exp = get_u32(at + 4),
                                      .flags = get_u32(at + 8),
                                      .battles = get_u16(at + 12),
                                      .turns = get_u16(at + 14)};
    if (!(progress.cleared >> c & 1u)) continue;
    if (clear->level < 1u || clear->level > H2_XIANXIA_RPG_LEVEL_MAX ||
        clear->pills > H2_XIANXIA_RPG_PILLS_MAX ||
        clear->trust > H2_XIANXIA_RPG_TRUST_MAX)
      return false;
  }
  *out = progress;
  return true;
}
