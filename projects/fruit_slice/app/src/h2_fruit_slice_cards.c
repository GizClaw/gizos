#include "h2_fruit_slice_cards.h"

#include "h2_fruit_slice_game.h"

#include <math.h>
#include <string.h>

#define CARDS_MAGIC 0x31435346u /* "FSC1" little-endian */
#define CARDS_VERSION 1u
#define CARDS_HEADER_BYTES 16u
#define BADGE_PULSE_MS 420u
#define TWINKLE_PERIOD_MS 170u
#define SPARK_GRAVITY 0.35f

static const h2_fruit_slice_card_t k_cards[H2_FRUIT_SLICE_CARD_COUNT] = {
    {"WATERMELON", "SLICE WATERMELONS", H2_FRUIT_SLICE_CARD_TAB_FRUIT,
     H2_FRUIT_SLICE_RARITY_COMMON},
    {"ORANGE", "SLICE ORANGES", H2_FRUIT_SLICE_CARD_TAB_FRUIT,
     H2_FRUIT_SLICE_RARITY_COMMON},
    {"APPLE", "SLICE APPLES", H2_FRUIT_SLICE_CARD_TAB_FRUIT,
     H2_FRUIT_SLICE_RARITY_COMMON},
    {"KIWI", "SLICE KIWIS", H2_FRUIT_SLICE_CARD_TAB_FRUIT,
     H2_FRUIT_SLICE_RARITY_RARE},
    {"LEMON", "SLICE LEMONS", H2_FRUIT_SLICE_CARD_TAB_FRUIT,
     H2_FRUIT_SLICE_RARITY_RARE},
    {"FRUIT SALAD", "SLICE 30 IN ONE RUN", H2_FRUIT_SLICE_CARD_TAB_FRUIT,
     H2_FRUIT_SLICE_RARITY_EPIC},
    {"CLASSIC KNIFE", "SCORE 10 WITH KNIFE", H2_FRUIT_SLICE_CARD_TAB_WEAPON,
     H2_FRUIT_SLICE_RARITY_COMMON},
    {"JADE KATANA", "SCORE 10 WITH KATANA", H2_FRUIT_SLICE_CARD_TAB_WEAPON,
     H2_FRUIT_SLICE_RARITY_RARE},
    {"FLAME AXE", "SCORE 10 WITH AXE", H2_FRUIT_SLICE_CARD_TAB_WEAPON,
     H2_FRUIT_SLICE_RARITY_EPIC},
    {"BOMB", "HIT A BOMB", H2_FRUIT_SLICE_CARD_TAB_BOMB,
     H2_FRUIT_SLICE_RARITY_COMMON},
    {"CLOSE CALL", "DODGE 3 BOMBS IN A RUN", H2_FRUIT_SLICE_CARD_TAB_BOMB,
     H2_FRUIT_SLICE_RARITY_RARE},
    {"BOMB SQUAD", "DODGE 8 BOMBS IN A RUN", H2_FRUIT_SLICE_CARD_TAB_BOMB,
     H2_FRUIT_SLICE_RARITY_EPIC},
    {"COMBO", "SLICE 3 IN ONE SWIPE", H2_FRUIT_SLICE_CARD_TAB_SKILL,
     H2_FRUIT_SLICE_RARITY_COMMON},
    {"MEGA COMBO", "SLICE 5 IN ONE SWIPE", H2_FRUIT_SLICE_CARD_TAB_SKILL,
     H2_FRUIT_SLICE_RARITY_EPIC},
    {"HIGH SCORE", "SCORE 30 IN ONE RUN", H2_FRUIT_SLICE_CARD_TAB_SKILL,
     H2_FRUIT_SLICE_RARITY_RARE},
    {"FRUIT LEGEND", "SCORE 60 IN ONE RUN", H2_FRUIT_SLICE_CARD_TAB_SKILL,
     H2_FRUIT_SLICE_RARITY_LEGENDARY},
};

static const char* const k_tab_names[H2_FRUIT_SLICE_CARD_TABS] = {
    "FRUIT", "WEAPON", "BOMB", "SKILL"};

static const char* const k_rarity_names[H2_FRUIT_SLICE_RARITY_COUNT] = {
    "COMMON", "RARE", "EPIC", "LEGENDARY"};

const h2_fruit_slice_card_t* h2_fruit_slice_card(unsigned id) {
  return &k_cards[id < H2_FRUIT_SLICE_CARD_COUNT ? id : 0u];
}

uint16_t h2_fruit_slice_card_rarity_color(unsigned rarity) {
  switch (rarity) {
    case H2_FRUIT_SLICE_RARITY_RARE:
      return H2_FRUIT_SLICE_RGB(72, 168, 255);
    case H2_FRUIT_SLICE_RARITY_EPIC:
      return H2_FRUIT_SLICE_RGB(196, 98, 255);
    case H2_FRUIT_SLICE_RARITY_LEGENDARY:
      return H2_FRUIT_SLICE_RGB(255, 200, 40);
    default:
      return H2_FRUIT_SLICE_RGB(196, 206, 216);
  }
}

const char* h2_fruit_slice_card_rarity_name(unsigned rarity) {
  return k_rarity_names[rarity < H2_FRUIT_SLICE_RARITY_COUNT ? rarity : 0u];
}

const char* h2_fruit_slice_card_tab_name(unsigned tab) {
  return k_tab_names[tab < H2_FRUIT_SLICE_CARD_TABS ? tab : 0u];
}

unsigned h2_fruit_slice_card_tab_cards(
    unsigned tab, uint8_t out[H2_FRUIT_SLICE_CARD_TAB_SLOTS]) {
  unsigned count = 0;
  for (unsigned id = 0; id < H2_FRUIT_SLICE_CARD_COUNT; ++id)
    if (k_cards[id].category == tab && count < H2_FRUIT_SLICE_CARD_TAB_SLOTS)
      out[count++] = (uint8_t)id;
  return count;
}

static unsigned popcount32(uint32_t value) {
  unsigned count = 0;
  for (; value != 0u; value &= value - 1u) ++count;
  return count;
}

unsigned h2_fruit_slice_cards_run_landed(const h2_fruit_slice_cards_t* cards) {
  unsigned airborne = 0u;
  for (unsigned i = 0; i < H2_FRUIT_SLICE_CARD_FLIGHTS; ++i)
    if (cards->flights[i].active) ++airborne;
  return popcount32(cards->run_earned) - airborne;
}

static uint32_t random_next(h2_fruit_slice_cards_t* cards) {
  uint32_t x = cards->rng;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  cards->rng = x;
  return x;
}

static float random_range(h2_fruit_slice_cards_t* cards, float low,
                          float high) {
  return low +
         (high - low) * (float)(random_next(cards) >> 8) * (1.0f / 16777216.0f);
}

void h2_fruit_slice_cards_init(h2_fruit_slice_cards_t* cards, int32_t width,
                               int32_t height, uint32_t seed) {
  memset(cards, 0, sizeof(*cards));
  cards->width = width > 0 ? width : 1;
  cards->height = height > 0 ? height : 1;
  const int32_t short_side =
      cards->width < cards->height ? cards->width : cards->height;
  cards->unit = (float)short_side / 480.0f;
  /* Independent of the game's generator so card rolls never change the
   * thrown-fruit sequence for a seed. */
  cards->rng = (seed ^ 0x5CA1AB1Eu) != 0u ? seed ^ 0x5CA1AB1Eu : 0x2545F491u;
  cards->tab_direction = 1;
  cards->tab_ms = H2_FRUIT_SLICE_CARD_TAB_SLIDE_MS;
}

bool h2_fruit_slice_cards_owned(const h2_fruit_slice_cards_t* cards,
                                unsigned id) {
  return id < H2_FRUIT_SLICE_CARD_COUNT && cards->counts[id] != 0u;
}

unsigned h2_fruit_slice_cards_owned_total(const h2_fruit_slice_cards_t* cards) {
  unsigned total = 0;
  for (unsigned id = 0; id < H2_FRUIT_SLICE_CARD_COUNT; ++id)
    if (cards->counts[id] != 0u) ++total;
  return total;
}

static uint32_t owned_mask(const uint8_t counts[H2_FRUIT_SLICE_CARD_COUNT]) {
  uint32_t mask = 0;
  for (unsigned id = 0; id < H2_FRUIT_SLICE_CARD_COUNT; ++id)
    if (counts[id] != 0u) mask |= 1u << id;
  return mask;
}

static void put_u32(uint8_t* out, uint32_t value) {
  for (unsigned byte = 0; byte < 4u; ++byte)
    out[byte] = (uint8_t)(value >> (byte * 8u));
}

static uint32_t get_u32(const uint8_t* in) {
  uint32_t value = 0;
  for (unsigned byte = 0; byte < 4u; ++byte)
    value |= (uint32_t)in[byte] << (byte * 8u);
  return value;
}

void h2_fruit_slice_cards_encode(const h2_fruit_slice_cards_t* cards,
                                 uint8_t out[H2_FRUIT_SLICE_CARDS_BYTES]) {
  memset(out, 0, H2_FRUIT_SLICE_CARDS_BYTES);
  put_u32(out, CARDS_MAGIC);
  put_u32(out + 4, CARDS_VERSION);
  put_u32(out + 8, cards->unseen & owned_mask(cards->counts));
  put_u32(out + 12, H2_FRUIT_SLICE_CARD_COUNT);
  for (unsigned id = 0; id < H2_FRUIT_SLICE_CARD_COUNT; ++id)
    out[CARDS_HEADER_BYTES + id] = cards->counts[id];
}

/* Parse into counts/unseen without touching the collection. */
static bool parse(const void* data, size_t size,
                  uint8_t counts[H2_FRUIT_SLICE_CARD_COUNT], uint32_t* unseen) {
  if (data == NULL || size != H2_FRUIT_SLICE_CARDS_BYTES) return false;
  const uint8_t* bytes = data;
  const uint32_t slots = get_u32(bytes + 12);
  if (get_u32(bytes) != CARDS_MAGIC || get_u32(bytes + 4) != CARDS_VERSION ||
      slots != H2_FRUIT_SLICE_CARD_COUNT)
    return false;
  memset(counts, 0, H2_FRUIT_SLICE_CARD_COUNT);
  for (unsigned id = 0; id < slots; ++id) {
    const uint8_t count = bytes[CARDS_HEADER_BYTES + id];
    if (count > H2_FRUIT_SLICE_CARD_MAX_COUNT) return false;
    counts[id] = count;
  }
  for (unsigned i = CARDS_HEADER_BYTES + slots; i < size; ++i)
    if (bytes[i] != 0u) return false;
  *unseen = get_u32(bytes + 8);
  if ((*unseen & ~owned_mask(counts)) != 0u) return false;
  return true;
}

bool h2_fruit_slice_cards_decode(h2_fruit_slice_cards_t* cards,
                                 const void* data, size_t size) {
  uint8_t counts[H2_FRUIT_SLICE_CARD_COUNT];
  uint32_t unseen = 0;
  if (cards == NULL || !parse(data, size, counts, &unseen)) return false;
  memcpy(cards->counts, counts, sizeof(counts));
  cards->unseen = unseen;
  cards->seen_before_load = 0u;
  cards->loaded = true;
  ++cards->revision;
  return true;
}

bool h2_fruit_slice_cards_merge(h2_fruit_slice_cards_t* cards, const void* data,
                                size_t size) {
  uint8_t counts[H2_FRUIT_SLICE_CARD_COUNT];
  uint32_t unseen = 0;
  if (cards == NULL || cards->loaded || !parse(data, size, counts, &unseen))
    return false;
  const uint32_t stored_owned = owned_mask(counts);
  for (unsigned id = 0; id < H2_FRUIT_SLICE_CARD_COUNT; ++id) {
    const unsigned total = (unsigned)cards->counts[id] + counts[id];
    cards->counts[id] = (uint8_t)(total > H2_FRUIT_SLICE_CARD_MAX_COUNT
                                      ? H2_FRUIT_SLICE_CARD_MAX_COUNT
                                      : total);
  }
  /* Stored copies retain their old NEW/seen state unless inspected locally.
   * A duplicate earned before the read must not make an old seen card NEW. */
  cards->unseen =
      ((cards->unseen & ~stored_owned) | unseen) & ~cards->seen_before_load;
  cards->seen_before_load = 0u;
  cards->run_new &= ~stored_owned;
  cards->reveal_queue &= ~stored_owned;
  if (cards->reveal_active) {
    /* Do not reset an in-flight flip or replay its cue. The visible card
     * finishes normally, even if recovery discovers it was a duplicate. */
    cards->reveal_queue |= 1u << cards->reveal_card;
    cards->reveal_total =
        (uint8_t)(cards->reveal_index + popcount32(cards->reveal_queue));
  }
  for (unsigned i = 0; i < H2_FRUIT_SLICE_CARD_FLIGHTS; ++i)
    if (cards->flights[i].active &&
        (stored_owned & (1u << cards->flights[i].card)) != 0u)
      cards->flights[i].duplicate = true;
  cards->loaded = true;
  ++cards->revision;
  return true;
}

static void push_cue(h2_fruit_slice_cards_t* cards,
                     h2_fruit_slice_card_cue_t cue) {
  if (cards->cue_count < H2_FRUIT_SLICE_CARD_CUES)
    cards->cues[cards->cue_count++] = (uint8_t)cue;
}

h2_fruit_slice_card_cue_t h2_fruit_slice_cards_take_cue(
    h2_fruit_slice_cards_t* cards) {
  if (cards->cue_count == 0u) return H2_FRUIT_SLICE_CARD_CUE_NONE;
  const h2_fruit_slice_card_cue_t cue =
      (h2_fruit_slice_card_cue_t)cards->cues[0];
  memmove(&cards->cues[0], &cards->cues[1], cards->cue_count - 1u);
  --cards->cue_count;
  return cue;
}

static void add_spark(h2_fruit_slice_cards_t* cards, float x, float y, float vx,
                      float vy, float size, uint16_t color, uint16_t life_ms) {
  h2_fruit_slice_card_spark_t* slot = &cards->sparks[0];
  for (unsigned i = 0; i < H2_FRUIT_SLICE_CARD_SPARKS; ++i) {
    h2_fruit_slice_card_spark_t* spark = &cards->sparks[i];
    if (!spark->active) {
      slot = spark;
      break;
    }
    /* Replace the spark nearest the end of its life. */
    if ((uint32_t)spark->life_ms - spark->age_ms <
        (uint32_t)slot->life_ms - slot->age_ms)
      slot = spark;
  }
  *slot = (h2_fruit_slice_card_spark_t){.active = true,
                                        .x = x,
                                        .y = y,
                                        .vx = vx,
                                        .vy = vy,
                                        .size = size,
                                        .color = color,
                                        .life_ms = life_ms};
}

static void spark_burst(h2_fruit_slice_cards_t* cards, float x, float y,
                        unsigned count, float speed, uint16_t color,
                        float size) {
  const float u = cards->unit;
  for (unsigned i = 0; i < count; ++i) {
    const float angle = ((float)i + random_range(cards, 0.0f, 0.8f)) *
                        (2.0f * H2_FRUIT_SLICE_PI / (float)count);
    const float v = random_range(cards, 0.45f, 1.0f) * speed * u;
    add_spark(cards, x, y, cosf(angle) * v, sinf(angle) * v,
              random_range(cards, 0.6f, 1.0f) * size * u,
              (i % 3u) == 0u ? H2_FRUIT_SLICE_RGB(255, 255, 255) : color,
              (uint16_t)(420u + random_next(cards) % 380u));
  }
}

static void clear_sparks(h2_fruit_slice_cards_t* cards) {
  memset(cards->sparks, 0, sizeof(cards->sparks));
}

static void add_flight(h2_fruit_slice_cards_t* cards, unsigned id,
                       bool duplicate, float x, float y) {
  h2_fruit_slice_card_flight_t* slot = &cards->flights[0];
  for (unsigned i = 0; i < H2_FRUIT_SLICE_CARD_FLIGHTS; ++i) {
    h2_fruit_slice_card_flight_t* flight = &cards->flights[i];
    if (!flight->active) {
      slot = flight;
      break;
    }
    if (flight->age_ms > slot->age_ms) slot = flight;
  }
  /* A replaced flight lands immediately so the badge count stays exact. */
  if (slot->active) cards->badge_pulse_ms = BADGE_PULSE_MS;
  const float margin = 24.0f * cards->unit;
  if (x < margin) x = margin;
  if (x > (float)cards->width - margin) x = (float)cards->width - margin;
  if (y < margin) y = margin;
  if (y > (float)cards->height - margin) y = (float)cards->height - margin;
  *slot = (h2_fruit_slice_card_flight_t){.active = true,
                                         .duplicate = duplicate,
                                         .card = (uint8_t)id,
                                         .x = x,
                                         .y = y};
}

static void award(h2_fruit_slice_cards_t* cards, unsigned id, float x,
                  float y) {
  const uint32_t bit = 1u << id;
  if (!cards->run_active || (cards->run_earned & bit) != 0u) return;
  cards->run_earned |= bit;
  const bool first = cards->counts[id] == 0u;
  if (cards->counts[id] < H2_FRUIT_SLICE_CARD_MAX_COUNT) ++cards->counts[id];
  if (first) {
    cards->run_new |= bit;
    cards->reveal_queue |= bit;
    cards->unseen |= bit;
  }
  cards->dirty = true;
  ++cards->revision;
  add_flight(cards, id, !first, x, y);
  spark_burst(cards, x, y, first ? 10u : 6u, 230.0f,
              h2_fruit_slice_card_rarity_color(k_cards[id].rarity), 4.0f);
  push_cue(cards, H2_FRUIT_SLICE_CARD_CUE_CARD);
}

void h2_fruit_slice_cards_run_start(h2_fruit_slice_cards_t* cards) {
  cards->run_active = true;
  cards->run_earned = cards->run_new = 0u;
  cards->run_slices = cards->run_bombs_passed = 0u;
  cards->badge_pulse_ms = 0u;
  memset(cards->flights, 0, sizeof(cards->flights));
  clear_sparks(cards);
  ++cards->revision;
}

static void check_score(h2_fruit_slice_cards_t* cards, uint32_t score,
                        unsigned weapon, float x, float y) {
  if (score >= H2_FRUIT_SLICE_CARD_WEAPON_SCORE && weapon < 3u)
    award(cards, H2_FRUIT_SLICE_CARD_CLASSIC_KNIFE + weapon, x, y);
  if (score >= H2_FRUIT_SLICE_CARD_HIGH_SCORE_TARGET)
    award(cards, H2_FRUIT_SLICE_CARD_HIGH_SCORE, x, y);
  if (score >= H2_FRUIT_SLICE_CARD_LEGEND_SCORE)
    award(cards, H2_FRUIT_SLICE_CARD_LEGEND, x, y);
}

void h2_fruit_slice_cards_on_slice(h2_fruit_slice_cards_t* cards,
                                   unsigned fruit_kind, uint32_t score,
                                   unsigned weapon, float x, float y) {
  if (!cards->run_active) return;
  ++cards->run_slices;
  if (fruit_kind < H2_FRUIT_SLICE_FRUIT_KINDS &&
      (cards->run_earned & (1u << fruit_kind)) == 0u) {
    const bool owned = cards->counts[fruit_kind] != 0u;
    const bool rare =
        k_cards[fruit_kind].rarity != H2_FRUIT_SLICE_RARITY_COMMON;
    const uint32_t percent =
        owned ? H2_FRUIT_SLICE_CARD_DROP_OWNED_PERCENT
              : (rare ? H2_FRUIT_SLICE_CARD_DROP_NEW_PERCENT / 2u
                      : H2_FRUIT_SLICE_CARD_DROP_NEW_PERCENT);
    if (cards->pity[fruit_kind] < 255u) ++cards->pity[fruit_kind];
    const bool pity =
        !owned && cards->pity[fruit_kind] >= H2_FRUIT_SLICE_CARD_DROP_PITY;
    if (pity || random_next(cards) % 100u < percent) {
      cards->pity[fruit_kind] = 0u;
      award(cards, fruit_kind, x, y);
    }
  }
  if (cards->run_slices >= H2_FRUIT_SLICE_CARD_SALAD_SLICES)
    award(cards, H2_FRUIT_SLICE_CARD_FRUIT_SALAD, x, y);
  check_score(cards, score, weapon, x, y);
}

void h2_fruit_slice_cards_on_combo(h2_fruit_slice_cards_t* cards,
                                   uint32_t slices, uint32_t score,
                                   unsigned weapon, float x, float y) {
  if (!cards->run_active) return;
  if (slices >= H2_FRUIT_SLICE_COMBO_MIN)
    award(cards, H2_FRUIT_SLICE_CARD_COMBO, x, y);
  if (slices >= H2_FRUIT_SLICE_CARD_MEGA_COMBO_SLICES)
    award(cards, H2_FRUIT_SLICE_CARD_MEGA_COMBO, x, y);
  check_score(cards, score, weapon, x, y);
}

void h2_fruit_slice_cards_on_bomb_hit(h2_fruit_slice_cards_t* cards, float x,
                                      float y) {
  award(cards, H2_FRUIT_SLICE_CARD_BOMB, x, y);
}

void h2_fruit_slice_cards_on_bomb_passed(h2_fruit_slice_cards_t* cards, float x,
                                         float y) {
  if (!cards->run_active) return;
  ++cards->run_bombs_passed;
  if (cards->run_bombs_passed >= H2_FRUIT_SLICE_CARD_CLOSE_CALL_BOMBS)
    award(cards, H2_FRUIT_SLICE_CARD_CLOSE_CALL, x, y);
  if (cards->run_bombs_passed >= H2_FRUIT_SLICE_CARD_SQUAD_BOMBS)
    award(cards, H2_FRUIT_SLICE_CARD_BOMB_SQUAD, x, y);
}

void h2_fruit_slice_cards_run_end(h2_fruit_slice_cards_t* cards) {
  if (!cards->run_active) return;
  cards->run_active = false;
  ++cards->revision;
}

void h2_fruit_slice_cards_reveal_layout(int32_t width, int32_t height,
                                        h2_fruit_slice_reveal_layout_t* out) {
  const float w = (float)width, h = (float)height;
  float card_h = h * 0.46f;
  if (card_h * 0.7f > w * 0.6f) card_h = w * 0.6f / 0.7f;
  *out = (h2_fruit_slice_reveal_layout_t){
      .cx = w * 0.5f,
      .cy = h * 0.47f,
      .card_w = card_h * 0.7f,
      .card_h = card_h,
      .art_radius = floorf(card_h * 0.7f * 0.27f)};
}

/* Lowest rarity first so the best card of the run is revealed last. */
static unsigned next_reveal(uint32_t queue) {
  for (unsigned rarity = 0; rarity < H2_FRUIT_SLICE_RARITY_COUNT; ++rarity)
    for (unsigned id = 0; id < H2_FRUIT_SLICE_CARD_COUNT; ++id)
      if ((queue & (1u << id)) != 0u && k_cards[id].rarity == rarity) return id;
  return 0u;
}

static void begin_reveal_card(h2_fruit_slice_cards_t* cards) {
  cards->reveal_card = (uint8_t)next_reveal(cards->reveal_queue);
  cards->reveal_ms = 0u;
  cards->reveal_cued = false;
}

/* The card turns face up: one cue and the rarity burst. */
static void reveal_turn(h2_fruit_slice_cards_t* cards) {
  if (cards->reveal_cued) return;
  cards->reveal_cued = true;
  const unsigned rarity = k_cards[cards->reveal_card].rarity;
  push_cue(cards, rarity == H2_FRUIT_SLICE_RARITY_COMMON
                      ? H2_FRUIT_SLICE_CARD_CUE_CARD
                      : H2_FRUIT_SLICE_CARD_CUE_RARE);
  h2_fruit_slice_reveal_layout_t l;
  h2_fruit_slice_cards_reveal_layout(cards->width, cards->height, &l);
  spark_burst(cards, l.cx, l.cy, 14u + 6u * rarity, 300.0f + 60.0f * rarity,
              h2_fruit_slice_card_rarity_color(rarity), 5.0f);
}

bool h2_fruit_slice_cards_reveal_active(const h2_fruit_slice_cards_t* cards) {
  return cards->reveal_active;
}

h2_fruit_slice_cards_action_t h2_fruit_slice_cards_reveal_pointer(
    h2_fruit_slice_cards_t* cards, int kind) {
  if (!cards->reveal_active) return H2_FRUIT_SLICE_CARDS_ACTION_NONE;
  /* Only a tap that started during the reveal counts, so lifting the finger
   * from the run's last swipe cannot skip the first card. */
  if (kind == H2_FRUIT_SLICE_POINTER_DOWN) {
    cards->pressing = true;
    return H2_FRUIT_SLICE_CARDS_ACTION_NONE;
  }
  if (kind != H2_FRUIT_SLICE_POINTER_UP || !cards->pressing) {
    if (kind == H2_FRUIT_SLICE_POINTER_CANCEL) cards->pressing = false;
    return H2_FRUIT_SLICE_CARDS_ACTION_NONE;
  }
  cards->pressing = false;
  if (cards->reveal_ms < H2_FRUIT_SLICE_REVEAL_FACE_MS) {
    /* First tap skips straight to the face-up card. */
    cards->reveal_ms = H2_FRUIT_SLICE_REVEAL_FACE_MS;
    reveal_turn(cards);
    return H2_FRUIT_SLICE_CARDS_ACTION_NONE;
  }
  if (cards->reveal_ms < H2_FRUIT_SLICE_REVEAL_INPUT_MS)
    return H2_FRUIT_SLICE_CARDS_ACTION_NONE;
  cards->reveal_queue &= ~(1u << cards->reveal_card);
  if (cards->reveal_queue != 0u) {
    ++cards->reveal_index;
    begin_reveal_card(cards);
    ++cards->revision;
    return H2_FRUIT_SLICE_CARDS_ACTION_NONE;
  }
  cards->reveal_active = false;
  clear_sparks(cards);
  ++cards->revision;
  return H2_FRUIT_SLICE_CARDS_ACTION_REVEAL_DONE;
}

void h2_fruit_slice_cards_album_layout(int32_t width, int32_t height,
                                       float unit,
                                       h2_fruit_slice_album_layout_t* out) {
  const float w = (float)width, h = (float)height;
  const bool landscape = w > h * 1.25f;
  memset(out, 0, sizeof(*out));
  out->title = (h2_fruit_slice_card_rect_t){0.0f, h * 0.025f, w, h * 0.10f};
  const float bar_y = landscape ? h * 0.135f : h * 0.112f;
  const float bar_h = fmaxf(6.0f, 10.0f * unit);
  out->progress = (h2_fruit_slice_card_rect_t){
      w * 0.18f, bar_y, w * (width < 200 ? 0.78f : 0.82f), bar_y + bar_h};
  const float tabs_y0 = landscape ? h * 0.19f : h * 0.158f;
  const float tabs_y1 = height < 200 ? h * 0.265f
                        : landscape  ? h * 0.285f
                                     : h * 0.225f;
  const float tabs_x0 = w * 0.03f, tabs_x1 = w * 0.97f, gap = w * 0.012f;
  const float tab_w =
      (tabs_x1 - tabs_x0 - gap * (float)(H2_FRUIT_SLICE_CARD_TABS - 1u)) /
      (float)H2_FRUIT_SLICE_CARD_TABS;
  for (unsigned i = 0; i < H2_FRUIT_SLICE_CARD_TABS; ++i) {
    const float x0 = tabs_x0 + (float)i * (tab_w + gap);
    out->tabs[i] =
        (h2_fruit_slice_card_rect_t){x0, tabs_y0, x0 + tab_w, tabs_y1};
  }
  h2_fruit_slice_rect_t back;
  h2_fruit_slice_game_back_rect(width, height, &back);
  const float gap_y = fmaxf(4.0f, 6.0f * unit);
  const float bottom = back.y0 - gap_y;
  h2_fruit_slice_card_rect_t grid;
  if (landscape) {
    grid =
        (h2_fruit_slice_card_rect_t){w * 0.02f, h * 0.30f, w * 0.62f, bottom};
    out->detail =
        (h2_fruit_slice_card_rect_t){w * 0.64f, h * 0.31f, w * 0.98f, bottom};
  } else {
    const float detail_h =
        height < 280 ? 30.0f : fminf(h * 0.135f, 96.0f * unit);
    const float detail_top = bottom - detail_h;
    grid = (h2_fruit_slice_card_rect_t){w * 0.03f,
                                        h * (height < 200 ? 0.28f : 0.24f),
                                        w * 0.97f, detail_top - gap_y};
    out->detail =
        (h2_fruit_slice_card_rect_t){w * 0.04f, detail_top, w * 0.96f, bottom};
  }
  const float cell_w = (grid.x1 - grid.x0) / 3.0f;
  const float cell_h = (grid.y1 - grid.y0) / 2.0f;
  float card_h = cell_h * 0.9f;
  if (card_h * 0.7f > cell_w * 0.9f) card_h = cell_w * 0.9f / 0.7f;
  out->card_h = floorf(card_h);
  out->card_w = floorf(card_h * 0.7f);
  out->art_radius = floorf(out->card_w * 0.27f);
  for (unsigned i = 0; i < H2_FRUIT_SLICE_CARD_TAB_SLOTS; ++i) {
    const float cx = grid.x0 + cell_w * ((float)(i % 3u) + 0.5f);
    const float cy = grid.y0 + cell_h * ((float)(i / 3u) + 0.5f);
    out->slots[i] =
        (h2_fruit_slice_card_rect_t){cx - cell_w * 0.46f, cy - cell_h * 0.46f,
                                     cx + cell_w * 0.46f, cy + cell_h * 0.46f};
  }
}

int h2_fruit_slice_cards_slot_card(const h2_fruit_slice_cards_t* cards,
                                   unsigned slot) {
  uint8_t ids[H2_FRUIT_SLICE_CARD_TAB_SLOTS];
  const unsigned count = h2_fruit_slice_card_tab_cards(cards->tab, ids);
  return slot < count ? (int)ids[slot] : -1;
}

bool h2_fruit_slice_cards_tab_sliding(const h2_fruit_slice_cards_t* cards) {
  return cards->album_open && cards->tab_ms < H2_FRUIT_SLICE_CARD_TAB_SLIDE_MS;
}

static void select_slot(h2_fruit_slice_cards_t* cards, unsigned slot) {
  const int id = h2_fruit_slice_cards_slot_card(cards, slot);
  if (id < 0) return;
  cards->selected = (uint8_t)slot;
  cards->select_ms = 0u;
  const uint32_t bit = 1u << (unsigned)id;
  if (!cards->loaded && cards->counts[id] != 0u) cards->seen_before_load |= bit;
  if ((cards->unseen & bit) != 0u) {
    cards->unseen &= ~bit;
    cards->dirty = true;
  }
  ++cards->revision;
}

static void set_tab(h2_fruit_slice_cards_t* cards, unsigned tab,
                    int direction) {
  cards->tab = (uint8_t)(tab % H2_FRUIT_SLICE_CARD_TABS);
  cards->tab_direction = (int8_t)(direction < 0 ? -1 : 1);
  cards->tab_ms = 0u;
  /* Sparks belong to the previous tab's selection. */
  clear_sparks(cards);
  select_slot(cards, 0u);
}

void h2_fruit_slice_cards_open_album(h2_fruit_slice_cards_t* cards) {
  cards->album_open = true;
  cards->album_ms = 0u;
  cards->pressing = false;
  for (unsigned id = 0; id < H2_FRUIT_SLICE_CARD_COUNT; ++id) {
    if ((cards->unseen & (1u << id)) == 0u) continue;
    uint8_t ids[H2_FRUIT_SLICE_CARD_TAB_SLOTS];
    const unsigned tab = k_cards[id].category;
    const unsigned count = h2_fruit_slice_card_tab_cards(tab, ids);
    for (unsigned slot = 0; slot < count; ++slot)
      if (ids[slot] == id) {
        set_tab(cards, tab, 1);
        select_slot(cards, slot);
        return;
      }
  }
  const unsigned selected = cards->selected;
  set_tab(cards, cards->tab, 1);
  select_slot(cards, selected);
}

void h2_fruit_slice_cards_close_album(h2_fruit_slice_cards_t* cards) {
  cards->album_open = false;
  cards->pressing = false;
  clear_sparks(cards);
  ++cards->revision;
}

static bool inside(const h2_fruit_slice_card_rect_t* r, float x, float y) {
  return x >= r->x0 && x <= r->x1 && y >= r->y0 && y <= r->y1;
}

void h2_fruit_slice_cards_album_pointer(h2_fruit_slice_cards_t* cards, int kind,
                                        float x, float y) {
  if (!cards->album_open) return;
  if (kind == H2_FRUIT_SLICE_POINTER_DOWN) {
    cards->pressing = true;
    cards->press_x = x;
    cards->press_y = y;
    return;
  }
  if (kind == H2_FRUIT_SLICE_POINTER_CANCEL) {
    cards->pressing = false;
    return;
  }
  if (kind != H2_FRUIT_SLICE_POINTER_UP || !cards->pressing) return;
  cards->pressing = false;
  const float dx = x - cards->press_x, dy = y - cards->press_y;
  if (fabsf(dx) > (float)cards->width * 0.15f && fabsf(dx) > fabsf(dy) * 1.5f) {
    /* Swipe left for the next tab, right for the previous one. */
    if (dx < 0.0f)
      set_tab(cards, (cards->tab + 1u) % H2_FRUIT_SLICE_CARD_TABS, 1);
    else
      set_tab(cards,
              (cards->tab + H2_FRUIT_SLICE_CARD_TABS - 1u) %
                  H2_FRUIT_SLICE_CARD_TABS,
              -1);
    return;
  }
  h2_fruit_slice_album_layout_t l;
  h2_fruit_slice_cards_album_layout(cards->width, cards->height, cards->unit,
                                    &l);
  for (unsigned tab = 0; tab < H2_FRUIT_SLICE_CARD_TABS; ++tab)
    if (inside(&l.tabs[tab], x, y) &&
        inside(&l.tabs[tab], cards->press_x, cards->press_y)) {
      if (tab != cards->tab) set_tab(cards, tab, tab > cards->tab ? 1 : -1);
      return;
    }
  for (unsigned slot = 0; slot < H2_FRUIT_SLICE_CARD_TAB_SLOTS; ++slot)
    if (inside(&l.slots[slot], x, y) &&
        inside(&l.slots[slot], cards->press_x, cards->press_y)) {
      select_slot(cards, slot);
      return;
    }
}

static uint16_t age16(uint16_t value, uint32_t ms) {
  const uint32_t next = (uint32_t)value + ms;
  return next > 0xFFFFu ? 0xFFFFu : (uint16_t)next;
}

static void step_sparks(h2_fruit_slice_cards_t* cards, uint32_t ms) {
  const float dt = (float)ms / 1000.0f;
  const float gravity = SPARK_GRAVITY * 1.5f * (float)cards->height;
  for (unsigned i = 0; i < H2_FRUIT_SLICE_CARD_SPARKS; ++i) {
    h2_fruit_slice_card_spark_t* s = &cards->sparks[i];
    if (!s->active) continue;
    s->vy += gravity * dt;
    s->vx *= 1.0f - 1.6f * dt;
    s->x += s->vx * dt;
    s->y += s->vy * dt;
    s->age_ms = age16(s->age_ms, ms);
    if (s->age_ms >= s->life_ms) s->active = false;
  }
}

static void step_flights(h2_fruit_slice_cards_t* cards, uint32_t ms) {
  for (unsigned i = 0; i < H2_FRUIT_SLICE_CARD_FLIGHTS; ++i) {
    h2_fruit_slice_card_flight_t* f = &cards->flights[i];
    if (!f->active) continue;
    const uint16_t before = f->age_ms;
    f->age_ms = age16(f->age_ms, ms);
    /* A short glitter trail behind the travelling card. */
    if (f->age_ms > 200u && before / 60u != f->age_ms / 60u &&
        f->age_ms < H2_FRUIT_SLICE_CARD_FLIGHT_MS - 120u) {
      const float t = ((float)f->age_ms - 200.0f) /
                      (float)(H2_FRUIT_SLICE_CARD_FLIGHT_MS - 200u);
      const float tx = (float)cards->width * 0.5f;
      const float ty = 34.0f * cards->unit;
      const float cx = (f->x + tx) * 0.5f;
      const float cy = fminf(f->y, ty) - 60.0f * cards->unit;
      const float a = 1.0f - t;
      const float x = a * a * f->x + 2.0f * a * t * cx + t * t * tx;
      const float y = a * a * f->y + 2.0f * a * t * cy + t * t * ty;
      add_spark(cards, x, y, random_range(cards, -40.0f, 40.0f) * cards->unit,
                random_range(cards, -20.0f, 30.0f) * cards->unit,
                random_range(cards, 1.6f, 3.0f) * cards->unit,
                h2_fruit_slice_card_rarity_color(k_cards[f->card].rarity),
                360u);
    }
    if (f->age_ms >= H2_FRUIT_SLICE_CARD_FLIGHT_MS) {
      f->active = false;
      ++cards->revision;
      cards->badge_pulse_ms = BADGE_PULSE_MS;
      spark_burst(
          cards, (float)cards->width * 0.5f, 34.0f * cards->unit, 6u, 140.0f,
          h2_fruit_slice_card_rarity_color(k_cards[f->card].rarity), 3.0f);
    }
  }
  cards->badge_pulse_ms =
      cards->badge_pulse_ms > ms ? (uint16_t)(cards->badge_pulse_ms - ms) : 0u;
}

static void step_reveal(h2_fruit_slice_cards_t* cards, uint32_t ms,
                        bool allow_reveal) {
  if (!cards->reveal_active) {
    if (cards->reveal_queue == 0u || !allow_reveal || cards->album_open) return;
    cards->reveal_active = true;
    cards->pressing = false;
    cards->reveal_index = 0u;
    cards->reveal_total = (uint8_t)popcount32(cards->reveal_queue);
    memset(cards->flights, 0, sizeof(cards->flights));
    clear_sparks(cards);
    begin_reveal_card(cards);
    ++cards->revision;
    return;
  }
  const uint32_t before = cards->reveal_ms;
  cards->reveal_ms = before + ms > 600000u ? 600000u : before + ms;
  const unsigned rarity = k_cards[cards->reveal_card].rarity;
  h2_fruit_slice_reveal_layout_t l;
  h2_fruit_slice_cards_reveal_layout(cards->width, cards->height, &l);
  const uint16_t color = h2_fruit_slice_card_rarity_color(rarity);
  /* Charge: motes are drawn into the card before it flips. */
  if (cards->reveal_ms >= H2_FRUIT_SLICE_REVEAL_ZOOM_MS &&
      cards->reveal_ms < H2_FRUIT_SLICE_REVEAL_FLIP_START_MS &&
      before / 50u != cards->reveal_ms / 50u) {
    for (unsigned i = 0; i < 1u + rarity; ++i) {
      const float angle = random_range(cards, 0.0f, 2.0f * H2_FRUIT_SLICE_PI);
      const float reach = l.card_h * random_range(cards, 0.75f, 1.0f);
      const float x = l.cx + cosf(angle) * reach;
      const float y = l.cy + sinf(angle) * reach * 0.8f;
      add_spark(cards, x, y, (l.cx - x) * 2.6f, (l.cy - y) * 2.6f - 30.0f,
                random_range(cards, 2.0f, 3.5f) * cards->unit, color, 330u);
    }
  }
  const uint32_t turn =
      H2_FRUIT_SLICE_REVEAL_FLIP_START_MS +
      (H2_FRUIT_SLICE_REVEAL_FACE_MS - H2_FRUIT_SLICE_REVEAL_FLIP_START_MS) /
          2u;
  if (cards->reveal_ms >= turn) reveal_turn(cards);
  /* Face up: rarer cards keep shedding glitter around the frame. */
  if (cards->reveal_ms >= H2_FRUIT_SLICE_REVEAL_FACE_MS && rarity > 0u &&
      before / (420u / rarity) != cards->reveal_ms / (420u / rarity)) {
    const float side = (random_next(cards) & 1u) ? 0.5f : -0.5f;
    add_spark(cards, l.cx + side * l.card_w * random_range(cards, 0.9f, 1.1f),
              l.cy + l.card_h * random_range(cards, -0.5f, 0.3f),
              side * random_range(cards, 20.0f, 70.0f) * cards->unit,
              -random_range(cards, 40.0f, 110.0f) * cards->unit,
              random_range(cards, 2.0f, 3.6f) * cards->unit, color, 700u);
  }
}

static void step_album(h2_fruit_slice_cards_t* cards, uint32_t ms) {
  if (!cards->album_open) return;
  const uint32_t before = cards->tab_ms;
  cards->tab_ms = before + ms > 600000u ? 600000u : before + ms;
  if (before < H2_FRUIT_SLICE_CARD_TAB_SLIDE_MS &&
      cards->tab_ms >= H2_FRUIT_SLICE_CARD_TAB_SLIDE_MS)
    ++cards->revision; /* The static layer takes the settled grid back. */
  const uint32_t select_before = cards->select_ms;
  cards->select_ms =
      select_before + ms > 600000u ? 600000u : select_before + ms;
  cards->album_ms =
      cards->album_ms + ms > 600000u ? 600000u : cards->album_ms + ms;
  const int id = h2_fruit_slice_cards_slot_card(cards, cards->selected);
  if (id < 0 || cards->counts[id] == 0u ||
      k_cards[id].rarity < H2_FRUIT_SLICE_RARITY_EPIC ||
      cards->tab_ms < H2_FRUIT_SLICE_CARD_TAB_SLIDE_MS ||
      select_before / TWINKLE_PERIOD_MS == cards->select_ms / TWINKLE_PERIOD_MS)
    return;
  h2_fruit_slice_album_layout_t l;
  h2_fruit_slice_cards_album_layout(cards->width, cards->height, cards->unit,
                                    &l);
  const h2_fruit_slice_card_rect_t* r = &l.slots[cards->selected];
  add_spark(cards, random_range(cards, r->x0, r->x1),
            random_range(cards, r->y0, r->y1),
            random_range(cards, -12.0f, 12.0f) * cards->unit,
            -random_range(cards, 30.0f, 70.0f) * cards->unit,
            random_range(cards, 1.6f, 2.8f) * cards->unit,
            h2_fruit_slice_card_rarity_color(k_cards[id].rarity), 650u);
}

void h2_fruit_slice_cards_step(h2_fruit_slice_cards_t* cards, uint32_t ms,
                               bool allow_reveal) {
  cards->time_ms += ms;
  step_sparks(cards, ms);
  step_flights(cards, ms);
  step_reveal(cards, ms, allow_reveal);
  step_album(cards, ms);
}
