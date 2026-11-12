#include "h2_g99_settings.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #condition); \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

/* In-memory pref with one namespace of u32 keys. */
typedef struct fake_entry {
  char key[16];
  uint32_t value;
  bool used;
} fake_entry_t;

typedef struct fake_pref {
  h2_pal_pref_namespace_t ns;
  /* The one namespace this store holds; others are rejected. */
  const char* name;
  bool exists;
  bool open;
  bool read_only;
  bool fail_set;
  int commits;
  int closes;
  fake_entry_t committed[8];
  fake_entry_t staged[8];
} fake_pref_t;

static fake_entry_t* find(fake_entry_t* entries, const char* key, bool create) {
  for (int i = 0; i < 8; ++i)
    if (entries[i].used && strcmp(entries[i].key, key) == 0) return &entries[i];
  if (!create) return NULL;
  for (int i = 0; i < 8; ++i) {
    if (!entries[i].used) {
      entries[i].used = true;
      (void)snprintf(entries[i].key, sizeof(entries[i].key), "%s", key);
      return &entries[i];
    }
  }
  return NULL;
}

static int fake_close(h2_pal_pref_namespace_t* ns) {
  fake_pref_t* pref = ns->user;
  pref->open = false;
  pref->closes++;
  memcpy(pref->staged, pref->committed, sizeof(pref->staged));
  return H2_PAL_OK;
}

static int fake_get_u32(h2_pal_pref_namespace_t* ns, const char* key,
                        uint32_t* out) {
  fake_pref_t* pref = ns->user;
  fake_entry_t* entry = find(pref->committed, key, false);
  if (entry == NULL) return H2_PAL_ERR_NOT_FOUND;
  *out = entry->value;
  return H2_PAL_OK;
}

static int fake_set_u32(h2_pal_pref_namespace_t* ns, const char* key,
                        uint32_t value) {
  fake_pref_t* pref = ns->user;
  if (pref->read_only) return H2_PAL_ERR_INVALID_STATE;
  if (pref->fail_set) return H2_PAL_ERR_IO;
  fake_entry_t* entry = find(pref->staged, key, true);
  if (entry == NULL) return H2_PAL_ERR_NO_SPACE;
  entry->value = value;
  return H2_PAL_OK;
}

static int fake_commit(h2_pal_pref_namespace_t* ns) {
  fake_pref_t* pref = ns->user;
  pref->commits++;
  memcpy(pref->committed, pref->staged, sizeof(pref->committed));
  pref->exists = true;
  return H2_PAL_OK;
}

static int fake_open(void* user, const char* name_space,
                     h2_pal_pref_open_mode_t mode,
                     h2_pal_pref_namespace_t** out) {
  fake_pref_t* pref = user;
  if (strcmp(name_space, pref->name) != 0) return H2_PAL_ERR_INVALID_ARG;
  if (mode == H2_PAL_PREF_OPEN_READ_ONLY && !pref->exists)
    return H2_PAL_ERR_NOT_FOUND;
  pref->open = true;
  pref->read_only = mode == H2_PAL_PREF_OPEN_READ_ONLY;
  memcpy(pref->staged, pref->committed, sizeof(pref->staged));
  *out = &pref->ns;
  return H2_PAL_OK;
}

static const h2_pal_pref_vtable_t kVtable = {.open = fake_open};

static void fake_init(fake_pref_t* pref, h2_pal_pref_api_t* api) {
  memset(pref, 0, sizeof(*pref));
  pref->name = H2_G99_SETTINGS_NAMESPACE;
  pref->ns.user = pref;
  pref->ns.close = fake_close;
  pref->ns.get_u32 = fake_get_u32;
  pref->ns.set_u32 = fake_set_u32;
  pref->ns.commit = fake_commit;
  api->user = pref;
  api->vtable = &kVtable;
}

static void test_defaults_and_ranges(void) {
  h2_g99_settings_t s;
  h2_g99_settings_defaults(&s);
  CHECK(s.volume_percent == 60u);
  CHECK(s.brightness_percent == 100u);
  CHECK(s.screen_timeout_index == 1u);
  CHECK(h2_g99_screen_timeout_ms(0u) == 15000u);
  CHECK(h2_g99_screen_timeout_ms(1u) == 30000u);
  CHECK(h2_g99_screen_timeout_ms(2u) == 60000u);
  CHECK(h2_g99_screen_timeout_ms(3u) == 300000u);
  CHECK(h2_g99_screen_timeout_ms(4u) == 0u);
  s = (h2_g99_settings_t){150u, 0u, 9u, 2u};
  h2_g99_settings_normalize(&s);
  CHECK(s.volume_percent == 100u);
  CHECK(s.brightness_percent == H2_G99_BRIGHTNESS_MIN_PERCENT);
  CHECK(s.screen_timeout_index == 1u);
  CHECK(s.haptics_enabled == 0u);
}

static void test_steps_clamp(void) {
  h2_g99_settings_t s;
  h2_g99_settings_defaults(&s);
  CHECK(h2_g99_settings_step_volume(&s, 1) == 70u);
  CHECK(h2_g99_settings_step_volume(&s, -1) == 60u);
  s.volume_percent = 100u;
  CHECK(h2_g99_settings_step_volume(&s, 1) == 100u);
  s.volume_percent = 0u;
  CHECK(h2_g99_settings_step_volume(&s, -1) == 0u);
  s.brightness_percent = 10u;
  CHECK(h2_g99_settings_step_brightness(&s, -1) == 10u);
  s.brightness_percent = 95u;
  CHECK(h2_g99_settings_step_brightness(&s, 1) == 100u);
}

static void test_persistence_round_trip(void) {
  fake_pref_t pref;
  h2_pal_pref_api_t api;
  fake_init(&pref, &api);
  h2_g99_settings_t loaded;
  /* A device that never saved settings loads defaults without error. */
  CHECK(h2_g99_settings_load(&api, NULL, &loaded) == H2_PAL_OK);
  CHECK(loaded.volume_percent == 60u);
  const h2_g99_settings_t saved = {30u, 80u, 4u, 1u};
  CHECK(h2_g99_settings_save(&api, NULL, &saved) == H2_PAL_OK);
  CHECK(pref.commits == 1);
  CHECK(!pref.open);
  CHECK(h2_g99_settings_load(&api, NULL, &loaded) == H2_PAL_OK);
  CHECK(loaded.volume_percent == 30u);
  CHECK(loaded.brightness_percent == 80u);
  CHECK(loaded.screen_timeout_index == 4u);
  CHECK(loaded.haptics_enabled == 1u);
  CHECK(!pref.open);
  /* Out-of-range stored values fall back to the defaults. */
  find(pref.committed, "volume", false)->value = 250u;
  find(pref.committed, "screen_off", false)->value = 7u;
  CHECK(h2_g99_settings_load(&api, NULL, &loaded) == H2_PAL_OK);
  CHECK(loaded.volume_percent == 60u);
  CHECK(loaded.brightness_percent == 80u);
  CHECK(loaded.screen_timeout_index == 1u);
  /* Saving normalizes; a stored brightness of zero is never written. */
  const h2_g99_settings_t dark = {10u, 0u, 0u, 0u};
  CHECK(h2_g99_settings_save(&api, NULL, &dark) == H2_PAL_OK);
  CHECK(find(pref.committed, "brightness", false)->value ==
        H2_G99_BRIGHTNESS_MIN_PERCENT);
}

static void test_failures(void) {
  h2_g99_settings_t loaded;
  CHECK(h2_g99_settings_load(NULL, NULL, &loaded) == H2_PAL_ERR_UNSUPPORTED);
  CHECK(loaded.volume_percent == 60u);
  CHECK(h2_g99_settings_load(NULL, NULL, NULL) == H2_PAL_ERR_INVALID_ARG);
  fake_pref_t pref;
  h2_pal_pref_api_t api;
  fake_init(&pref, &api);
  pref.fail_set = true;
  const h2_g99_settings_t value = {50u, 50u, 2u, 1u};
  CHECK(h2_g99_settings_save(&api, NULL, &value) == H2_PAL_ERR_IO);
  CHECK(pref.commits == 0);
  CHECK(!pref.open);
  CHECK(pref.closes == 1);
  CHECK(h2_g99_settings_save(NULL, NULL, &value) == H2_PAL_ERR_INVALID_ARG);
}

static void test_namespace_selection(void) {
  fake_pref_t pref;
  h2_pal_pref_api_t api;
  fake_init(&pref, &api);
  pref.name = "g99_e2e";
  const h2_g99_settings_t value = {20u, 70u, 3u, 0u};
  /* An E2E namespace receives the writes; the default one is never opened. */
  CHECK(h2_g99_settings_save(&api, NULL, &value) == H2_PAL_ERR_INVALID_ARG);
  CHECK(pref.commits == 0);
  CHECK(h2_g99_settings_save(&api, "g99_e2e", &value) == H2_PAL_OK);
  h2_g99_settings_t loaded;
  CHECK(h2_g99_settings_load(&api, "g99_e2e", &loaded) == H2_PAL_OK);
  CHECK(loaded.volume_percent == 20u && loaded.brightness_percent == 70u &&
        loaded.screen_timeout_index == 3u);
}

int main(void) {
  test_defaults_and_ranges();
  test_steps_clamp();
  test_persistence_round_trip();
  test_failures();
  test_namespace_selection();
  if (failures != 0) {
    fprintf(stderr, "%d settings check(s) failed\n", failures);
    return 1;
  }
  return 0;
}
