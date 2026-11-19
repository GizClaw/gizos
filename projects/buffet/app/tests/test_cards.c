#include "buffet_cards.h"
#undef NDEBUG /* Exercise the same checks in optimized host builds. */
#include <assert.h>
#include <string.h>

/* Model a provider with a different close/commit outcome from staging.
 * Actual Web and SQLite implementations are also exercised by replay. */
typedef struct {
  h2_pal_pref_namespace_t ns;
  uint32_t saved[3], staged[3];
  int bad, fail_commit, writes;
} memory_t;
static int index_of(const char* key) {
  const char* keys[] = {"base-bite", "base-settle", "story-hospital"};
  for (int i = 0; i < 3; i++)
    if (strcmp(keys[i], key) == 0) return i;
  return -1;
}
static int get(h2_pal_pref_namespace_t* ns, const char* key, uint32_t* value) {
  memory_t* m = ns->user;
  int i = index_of(key);
  assert(i >= 0);
  if (m->bad == i + 1) return H2_PAL_ERR_IO;
  if (!m->saved[i]) return H2_PAL_ERR_NOT_FOUND;
  *value = m->saved[i];
  return H2_PAL_OK;
}
static int set(h2_pal_pref_namespace_t* ns, const char* key, uint32_t value) {
  memory_t* m = ns->user;
  int i = index_of(key);
  assert(i >= 0 && value == 1);
  m->staged[i] = value;
  m->writes++;
  return H2_PAL_OK;
}
static int commit(h2_pal_pref_namespace_t* ns) {
  memory_t* m = ns->user;
  if (m->fail_commit) return H2_PAL_ERR_IO;
  memcpy(m->saved, m->staged, sizeof(m->saved));
  return H2_PAL_OK;
}
static int close_ns(h2_pal_pref_namespace_t* ns) {
  (void)ns;
  return H2_PAL_OK;
}
static int open_ns(void* user, const char* name, h2_pal_pref_open_mode_t mode,
                   h2_pal_pref_namespace_t** out) {
  (void)mode;
  assert(strcmp(name, "buffet_cards_v1") == 0);
  memory_t* m = user;
  memcpy(m->staged, m->saved, sizeof(m->saved));
  m->ns = (h2_pal_pref_namespace_t){.user = m,
                                    .get_u32 = get,
                                    .set_u32 = set,
                                    .close = close_ns,
                                    .commit = commit};
  *out = &m->ns;
  return H2_PAL_OK;
}
static int with_lock(void* user, h2_buffet_card_work_fn work, void* context) {
  int* calls = user;
  if (*calls < 0) return H2_PAL_ERR_WOULD_BLOCK;
  ++*calls;
  return work(context);
}
static void shared_save(void) {
  memory_t mem = {0};
  const h2_pal_pref_vtable_t vt = {.open = open_ns};
  h2_pal_pref_api_t pref = {&mem, &vt};
  int calls = 0;
  h2_buffet_config_t cfg = {.user = &calls, .with_card_lock = with_lock};
  bf_game_t first, stale;
  bf_init(&first, false);
  bf_init(&stale, false);
  unsigned a = 0, b = 0;
  assert(bf_cards_sync(&pref, &first.album, &a, &cfg) == H2_PAL_OK);
  assert(bf_cards_sync(&pref, &stale.album, &b, &cfg) == H2_PAL_OK);
  assert(calls == 0);
  first.phase = stale.phase = BF_CLOSING;
  stale.eaten = 1;
  bf_award(&first);
  bf_award(&stale);
  a = first.album.awaiting;
  b = stale.album.awaiting;
  assert(a == 2 && b == 3 && !first.album.fresh && !stale.album.fresh);
  assert(bf_cards_sync(&pref, &first.album, &a, &cfg) == H2_PAL_OK);
  assert(first.album.fresh == 2 && calls == 1 && mem.writes == 1);
  calls = -1;
  assert(bf_cards_sync(&pref, &stale.album, &b, &cfg) ==
             H2_PAL_ERR_WOULD_BLOCK &&
         b == 3);
  assert(!stale.album.fresh && !bf_reveal_duration(&stale));
  calls = 0;
  assert(bf_cards_sync(&pref, &stale.album, &b, &cfg) == H2_PAL_OK && b == 0);
  assert(stale.album.fresh == 1 && stale.album.owned == 3 && mem.writes == 2 &&
         calls == 1);
  bf_init(&stale, false);
  stale.phase = BF_CLOSING;
  stale.eaten = 2;
  bf_award(&stale);
  b = stale.album.awaiting;
  assert(bf_cards_sync(&pref, &stale.album, &b, &cfg) == H2_PAL_OK);
  assert(!stale.album.fresh && !bf_reveal_duration(&stale) &&
         stale.album.owned == 3 && mem.writes == 2);
}
int main(void) {
  shared_save();
  memory_t mem = {0};
  const h2_pal_pref_vtable_t vt = {.open = open_ns};
  h2_pal_pref_api_t pref = {&mem, &vt};
  bf_album_t album = {0};
  unsigned pending = 0;
  assert(bf_cards_sync(&pref, &album, &pending, NULL) == H2_PAL_OK &&
         !album.owned && !mem.writes);
  bf_game_t g;
  bf_init(&g, false);
  bf_action(&g, H2_BUFFET_CONFIRM);
  g.player.pos = bf_place(&g, 1).station;
  bf_action(&g, H2_BUFFET_CONFIRM);
  assert(g.eaten == 1);
  bf_step(&g, bf_rules.duration_ms, 0, 0);
  assert(g.phase == BF_CLOSING && !g.album.fresh && g.album.owned == 3 &&
         bf_reveal_duration(&g) == 0);
  pending = g.album.awaiting;
  mem.fail_commit = 1;
  assert(bf_cards_sync(&pref, &g.album, &pending, NULL) != H2_PAL_OK &&
         pending == 3 && g.album.unsaved && !g.album.fresh && !mem.saved[0]);
  mem.fail_commit = 0;
  assert(bf_cards_sync(&pref, &g.album, &pending, NULL) == H2_PAL_OK &&
         !pending && mem.saved[0] && mem.saved[1] && g.album.fresh == 3 &&
         bf_reveal_duration(&g) == 2000);
  bf_action(&g, H2_BUFFET_CONFIRM);
  assert(g.phase == BF_CLOSING && g.album.reveal_ms == 2000);
  bf_tap(&g, 94, 380);
  assert(g.album.open && g.album.this_meal);
  bf_tap(&g, 375, 45);
  assert(!g.album.this_meal);
  bf_action(&g, H2_BUFFET_CONFIRM);
  assert(!g.album.open);
  bf_game_t again;
  bf_init(&again, false);
  assert(bf_cards_sync(&pref, &again.album, &pending, NULL) == H2_PAL_OK &&
         again.album.owned == 3);
  bf_action(&again, H2_BUFFET_CONFIRM);
  bf_step(&again, bf_rules.duration_ms, 0, 0);
  assert(!again.album.fresh && again.album.owned == 3);
  bf_game_t empty;
  bf_init(&empty, false);
  bf_action(&empty, H2_BUFFET_CONFIRM);
  bf_step(&empty, bf_rules.duration_ms, 0, 0);
  assert(empty.album.awaiting == 2 && !empty.album.fresh);
  bf_init(&g, false);
  bf_action(&g, H2_BUFFET_CONFIRM);
  g.fullness_milli = 185000;
  g.eaten = 5;
  bf_step(&g, bf_rules.tolerance_ms, 0, 0);
  assert(g.phase == BF_HOSPITAL && g.album.awaiting == 4 && !g.album.fresh);
  pending = 4;
  int writes = mem.writes;
  mem.bad = 1;
  assert(bf_cards_sync(&pref, &g.album, &pending, NULL) != H2_PAL_OK &&
         mem.writes == writes && pending == 4);
  mem.bad = 0;
  mem.saved[0] = 57;
  assert(bf_cards_sync(&pref, &g.album, &pending, NULL) != H2_PAL_OK &&
         mem.saved[0] == 57 && mem.writes == writes);
  mem.saved[0] = 1;
  assert(bf_cards_sync(&pref, &g.album, &pending, NULL) == H2_PAL_OK &&
         !pending && g.album.owned == 7 && g.album.fresh == 4 &&
         mem.writes == writes + 1);
  pending = 7;
  assert(bf_cards_sync(&pref, &g.album, &pending, NULL) == H2_PAL_OK &&
         !pending && mem.writes == writes + 1);
  return 0;
}
