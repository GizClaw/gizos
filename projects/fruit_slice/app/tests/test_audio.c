#include "h2_fruit_slice_audio.h"
#include "h2_fruit_slice_game.h"
#include "h2_fruit_slice.h"
#include "h2_desktop_platform.h"
#include "h2_pal_unsupported.h"
#include "h2_sqlite.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); \
      exit(1);                                                \
    }                                                         \
  } while (0)

typedef struct fixture {
  h2_pal_audio_api_t api;
  h2_pal_audio_track_t track;
  h2_pal_pref_api_t pref;
  h2_pal_pref_namespace_t ns;
  unsigned starts, stops, opens, closes, writes, commits;
  unsigned active_tracks;
  unsigned accepted, rejected, accept_before_block, drains, drain_blocks;
  bool block_writes;
  int create_result, write_result;
  uint32_t preference;
  uint32_t peak;
  int16_t last_pcm[4096], first_pcm[4096];
  bool paced_sink;
  uint64_t now_ms, consumed_at_ms;
  uint32_t queued_ms, starved_ms, full_writes;
  h2_audio_pcm_format_t format;
} fixture_t;

static int get_info(void* user, h2_audio_info_t* info) {
  fixture_t* f = user;
  *info = (h2_audio_info_t){.available = 1,
                            .playback_supported = 1,
                            .playback_format = f->format,
                            .max_tracks = 1};
  return H2_PAL_OK;
}
static int start(void* user) {
  ++((fixture_t*)user)->starts;
  return H2_PAL_OK;
}
static int stop(void* user) {
  fixture_t* f = user;
  CHECK(f->active_tracks == 0);
  ++f->stops;
  return H2_PAL_OK;
}
static int close_track(h2_pal_audio_track_t* track) {
  fixture_t* f = track->user;
  CHECK(f->active_tracks == 1);
  f->active_tracks = 0;
  ++f->closes;
  return H2_PAL_OK;
}
static int write_track(h2_pal_audio_track_t* track,
                       const h2_audio_frame_t* frame, uint32_t timeout) {
  fixture_t* f = track->user;
  CHECK(timeout == 0 &&
        frame->bytes == (size_t)f->format.frame_samples_per_channel *
                            f->format.channels * sizeof(int16_t));
  CHECK(frame->samples_per_channel == f->format.frame_samples_per_channel);
  CHECK(frame->channels == f->format.channels &&
        frame->sample_rate_hz == f->format.sample_rate_hz);
  const int16_t* samples = frame->data;
  for (size_t i = 0; i < frame->bytes / sizeof(*samples); ++i) {
    const uint32_t magnitude =
        samples[i] < 0 ? (uint32_t)-(int32_t)samples[i] : (uint32_t)samples[i];
    if (magnitude > f->peak) f->peak = magnitude;
  }
  CHECK(frame->bytes <= sizeof(f->last_pcm));
  memcpy(f->last_pcm, frame->data, frame->bytes);
  if (f->writes == 0u) memcpy(f->first_pcm, frame->data, frame->bytes);
  ++f->writes;
  if (f->block_writes && f->accepted >= f->accept_before_block) {
    ++f->rejected;
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  if (f->paced_sink) {
    const uint64_t elapsed = f->now_ms - f->consumed_at_ms;
    f->consumed_at_ms = f->now_ms;
    if (elapsed > f->queued_ms) {
      f->starved_ms += (uint32_t)(elapsed - f->queued_ms);
      f->queued_ms = 0u;
    } else {
      f->queued_ms -= (uint32_t)elapsed;
    }
    if (f->queued_ms > 20u) {
      ++f->full_writes;
      return H2_PAL_ERR_WOULD_BLOCK;
    }
    f->queued_ms += 20u;
  }
  if (f->write_result == H2_PAL_OK) ++f->accepted;
  return f->write_result;
}
static int drain_track(h2_pal_audio_track_t* track, uint32_t timeout) {
  fixture_t* f = track->user;
  CHECK(timeout == 0u);
  ++f->drains;
  if (f->drain_blocks != 0u) {
    --f->drain_blocks;
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  return H2_PAL_OK; /* The unpaced fixture consumes accepted PCM immediately. */
}
static int create(void* user, const h2_audio_track_config_t* config,
                  h2_pal_audio_track_t** out) {
  fixture_t* f = user;
  CHECK(config->buffer_frames == 2 && config->volume_factor_milli == 1000);
  if (f->create_result) return f->create_result;
  CHECK(f->active_tracks == 0);
  f->active_tracks = 1;
  ++f->opens;
  f->track = (h2_pal_audio_track_t){.user = f,
                                    .write = write_track,
                                    .close = close_track,
                                    .drain = drain_track};
  *out = &f->track;
  return H2_PAL_OK;
}
static const h2_pal_audio_vtable_t audio_vtable = {.get_info = get_info,
                                                   .start_speaker = start,
                                                   .stop_speaker = stop,
                                                   .create_track = create};
static int pref_get(h2_pal_pref_namespace_t* ns, const char* key,
                    uint32_t* value) {
  CHECK(strcmp(key, "audio_v1") == 0);
  *value = ((fixture_t*)ns->user)->preference;
  return H2_PAL_OK;
}
static int pref_set(h2_pal_pref_namespace_t* ns, const char* key,
                    uint32_t value) {
  CHECK(strcmp(key, "audio_v1") == 0);
  ((fixture_t*)ns->user)->preference = value;
  return H2_PAL_OK;
}
static int pref_commit(h2_pal_pref_namespace_t* ns) {
  ++((fixture_t*)ns->user)->commits;
  return 0;
}
static int pref_close(h2_pal_pref_namespace_t* ns) {
  (void)ns;
  return 0;
}
static int pref_open(void* user, const char* name, h2_pal_pref_open_mode_t mode,
                     h2_pal_pref_namespace_t** out) {
  (void)mode;
  CHECK(strcmp(name, H2_FRUIT_SLICE_PREF_NAMESPACE) == 0);
  *out = &((fixture_t*)user)->ns;
  return 0;
}
static const h2_pal_pref_vtable_t pref_vtable = {.open = pref_open};

static h2_runtime_t runtime_for(fixture_t* f) {
  if (f->format.sample_rate_hz == 0u)
    f->format = (h2_audio_pcm_format_t){16000, 320, 1, H2_AUDIO_SAMPLE_S16LE};
  f->api = (h2_pal_audio_api_t){f, &audio_vtable};
  f->pref = (h2_pal_pref_api_t){f, &pref_vtable};
  f->ns = (h2_pal_pref_namespace_t){.user = f,
                                    .get_u32 = pref_get,
                                    .set_u32 = pref_set,
                                    .commit = pref_commit,
                                    .close = pref_close};
  return (h2_runtime_t){.audio = &f->api,
                        .pref = &f->pref,
                        .mem = h2_desktop_platform_default_allocator()};
}

static unsigned active(const h2_fruit_slice_audio_t* audio) {
  unsigned n = 0;
  for (unsigned i = 0; i < H2_FRUIT_SLICE_AUDIO_VOICES; ++i)
    n += audio->voices[i].active;
  return n;
}

static void test_bank_mute_and_lifecycle(void) {
  fixture_t f = {0};
  h2_runtime_t rt = runtime_for(&f);
  h2_fruit_slice_audio_t audio;
  CHECK(h2_fruit_slice_audio_init(&audio, &rt) == 0);
  CHECK(f.starts == 0); /* User gesture gate is owned by the App. */
  CHECK(h2_fruit_slice_audio_pump(&audio, 100) == 0);
  CHECK(f.starts == 1 && f.opens == 1 && f.peak > 0 && f.peak < 4000);
  CHECK(h2_fruit_slice_audio_set_mute(&audio, H2_FRUIT_SLICE_MUTE_MUSIC) == 0);
  CHECK(f.closes == 0 && f.stops == 0 && f.commits == 1);
  CHECK(h2_fruit_slice_audio_pump(&audio, 120) == H2_PAL_OK);
  for (unsigned sound = 0; sound < H2_FRUIT_SLICE_SOUND_COUNT; ++sound) {
    CHECK(h2_fruit_slice_audio_suspend(&audio) == 0);
    f.peak = 0;
    h2_fruit_slice_audio_emit(&audio, (h2_fruit_slice_sound_t)sound,
                              1000 + sound * 1000);
    CHECK(h2_fruit_slice_audio_pump(&audio, 1000 + sound * 1000) == 0);
    CHECK(f.peak > 0); /* Every shipped event actually produces PCM. */
  }
  CHECK(h2_fruit_slice_audio_set_mute(&audio, 3) == 0);
  h2_fruit_slice_audio_emit(&audio, H2_FRUIT_SLICE_SOUND_BOMB, 50000);
  CHECK(h2_fruit_slice_audio_pump(&audio, 50000) == H2_PAL_OK);
  CHECK(h2_fruit_slice_audio_pump(&audio, 50020) == H2_PAL_OK);
  CHECK(active(&audio) == 0 && audio.track == NULL);
  CHECK(h2_fruit_slice_audio_close(&audio) == 0);
  CHECK(f.opens == f.closes && f.starts == f.stops);
  CHECK(h2_fruit_slice_audio_init(&audio, &rt) == 0 && audio.mute == 3);
  CHECK(h2_fruit_slice_audio_close(&audio) == 0);
  f.preference = 0xffffffff; /* Corrupt/foreign version defaults safely. */
  CHECK(h2_fruit_slice_audio_init(&audio, &rt) == 0 && audio.mute == 0);
  CHECK(h2_fruit_slice_audio_close(&audio) == 0);
}

static void test_bursts_backpressure_and_partial_failure(void) {
  fixture_t f = {0};
  h2_runtime_t rt = runtime_for(&f);
  h2_fruit_slice_audio_t audio;
  CHECK(h2_fruit_slice_audio_init(&audio, &rt) == 0);
  for (unsigned i = 0; i < 200; ++i)
    h2_fruit_slice_audio_emit(&audio, H2_FRUIT_SLICE_SOUND_SLICE, 100);
  CHECK(active(&audio) == 1);
  h2_fruit_slice_audio_emit(&audio, H2_FRUIT_SLICE_SOUND_CARD, 100);
  h2_fruit_slice_audio_emit(&audio, H2_FRUIT_SLICE_SOUND_COMBO, 100);
  h2_fruit_slice_audio_emit(&audio, H2_FRUIT_SLICE_SOUND_TASK, 100);
  CHECK(active(&audio) == 4);
  h2_fruit_slice_audio_emit(&audio, H2_FRUIT_SLICE_SOUND_BOMB, 100);
  CHECK(active(&audio) == 1);
  f.write_result = H2_PAL_ERR_WOULD_BLOCK;
  CHECK(h2_fruit_slice_audio_pump(&audio, 100) == 0);
  CHECK(audio.music_position == 0u && audio.voices[0].position == 0u &&
        audio.voices[0].active && audio.submitted_samples == 0u);
  unsigned writes = f.writes;
  CHECK(h2_fruit_slice_audio_pump(&audio, 100000) == 0);
  CHECK(f.writes == writes + 1); /* No long catch-up after a stall. */
  f.write_result = H2_PAL_OK;
  CHECK(h2_fruit_slice_audio_pump(&audio, 100020) == H2_PAL_OK);
  CHECK(audio.available && audio.track != NULL && f.opens == 1u);
  f.write_result = H2_PAL_ERR_IO;
  CHECK(h2_fruit_slice_audio_pump(&audio, 100040) == H2_PAL_ERR_IO);
  CHECK(h2_fruit_slice_audio_suspend(&audio) == 0);
  CHECK(active(&audio) == 0 && !audio.track && !audio.speaker_started);
  f.create_result = H2_PAL_ERR_NO_MEMORY;
  CHECK(h2_fruit_slice_audio_pump(&audio, 200000) == H2_PAL_ERR_NO_MEMORY);
  CHECK(!audio.speaker_started && f.starts == f.stops);
  CHECK(h2_fruit_slice_audio_close(&audio) == 0);
  rt.audio = h2_pal_unsupported_audio_api();
  CHECK(h2_fruit_slice_audio_init(&audio, &rt) == H2_PAL_ERR_UNSUPPORTED);
  CHECK(h2_fruit_slice_audio_close(&audio) == 0);
}

static void test_resampling_and_music_loop(void) {
  fixture_t f = {.format = {48000, 960, 2, H2_AUDIO_SAMPLE_S16LE}};
  h2_runtime_t rt = runtime_for(&f);
  h2_fruit_slice_audio_t audio;
  CHECK(h2_fruit_slice_audio_init(&audio, &rt) == 0);
  for (uint64_t now = 0; now <= 10000; now += 20)
    CHECK(h2_fruit_slice_audio_pump(&audio, now) == 0);
  CHECK(f.writes == 502u && f.peak > 0 && f.peak < 4000u);
  CHECK(h2_fruit_slice_audio_set_mute(&audio, H2_FRUIT_SLICE_MUTE_SFX) == 0);
  h2_fruit_slice_audio_emit(&audio, H2_FRUIT_SLICE_SOUND_BOMB, 11000);
  CHECK(active(&audio) == 0);
  CHECK(h2_fruit_slice_audio_close(&audio) == 0);
  f.format = (h2_audio_pcm_format_t){8000, 160, 1, H2_AUDIO_SAMPLE_S16LE};
  CHECK(h2_fruit_slice_audio_init(&audio, &rt) == 0);
  CHECK(h2_fruit_slice_audio_pump(&audio, 12000) == 0);
  CHECK(h2_fruit_slice_audio_close(&audio) == 0);
}

static void test_sqlite_first_session_preferences(void) {
  const char* temporary = getenv("TEST_TMPDIR");
  CHECK(temporary != NULL);
  char database[1024];
  CHECK(snprintf(database, sizeof(database), "%s/audio-first-session.sqlite",
                 temporary) > 0);
  (void)remove(database);
  h2_sqlite_t* pref = NULL;
  const h2_sqlite_config_t config = {.path = database};
  CHECK(h2_sqlite_create(&config, &pref) == H2_PAL_OK);
  h2_pal_pref_namespace_t* ns = NULL;
  CHECK(h2_pal_pref_open(h2_sqlite_pref_api(pref),
                         H2_FRUIT_SLICE_PREF_NAMESPACE,
                         H2_PAL_PREF_OPEN_READ_ONLY, &ns) == H2_PAL_ERR_IO);
  fixture_t f = {0};
  h2_runtime_t rt = runtime_for(&f);
  rt.pref = h2_sqlite_pref_api(pref);
  h2_fruit_slice_audio_t audio;
  CHECK(h2_fruit_slice_audio_init(&audio, &rt) == H2_PAL_OK);
  CHECK(audio.mute == 0u);
  /* A missing database on the initial RO load cannot latch a permanent
   * read-only state: the first change must create and commit the database. */
  CHECK(h2_fruit_slice_audio_set_mute(&audio, H2_FRUIT_SLICE_MUTE_MUSIC) ==
        H2_PAL_OK);
  CHECK(h2_fruit_slice_audio_close(&audio) == H2_PAL_OK);
  h2_sqlite_destroy(pref);
  CHECK(h2_sqlite_create(&config, &pref) == H2_PAL_OK);
  rt.pref = h2_sqlite_pref_api(pref);
  CHECK(h2_fruit_slice_audio_init(&audio, &rt) == H2_PAL_OK);
  CHECK(audio.mute == H2_FRUIT_SLICE_MUTE_MUSIC);
  CHECK(h2_fruit_slice_audio_close(&audio) == H2_PAL_OK);
  h2_sqlite_destroy(pref);
  CHECK(remove(database) == 0);
}

static bool take(h2_fruit_slice_game_t* game, h2_fruit_slice_sound_t wanted) {
  h2_fruit_slice_sound_t sound;
  bool found = false;
  while (h2_fruit_slice_game_take_sound(game, &sound)) found |= sound == wanted;
  return found;
}
static void test_actual_game_transitions(void) {
  h2_fruit_slice_game_t game;
  h2_fruit_slice_game_init(&game, 480, 480, 42, 0);
  h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_DOWN, 100, 288);
  h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_MOVE, 380, 288);
  CHECK(game.phase == H2_FRUIT_SLICE_PHASE_PLAYING);
  CHECK(take(&game, H2_FRUIT_SLICE_SOUND_SLICE));
  memset(game.fruits, 0, sizeof(game.fruits));
  game.fruits[0] = (h2_fruit_slice_fruit_t){.active = true,
                                            .kind = H2_FRUIT_SLICE_BOMB,
                                            .radius = 30,
                                            .body = {.x = 240, .y = 250}};
  h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_DOWN, 100, 250);
  h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_MOVE, 380, 250);
  CHECK(game.phase == H2_FRUIT_SLICE_PHASE_GAME_OVER);
  CHECK(take(&game, H2_FRUIT_SLICE_SOUND_BOMB));
  h2_fruit_slice_game_step(&game, H2_FRUIT_SLICE_RESTART_DELAY_MS);
  CHECK(take(&game, H2_FRUIT_SLICE_SOUND_RESULT));
  h2_fruit_slice_game_step(&game, 100);
  CHECK(!take(&game, H2_FRUIT_SLICE_SOUND_RESULT));
  for (unsigned i = 0; i < H2_FRUIT_SLICE_SOUND_QUEUE; ++i)
    CHECK(h2_fruit_slice_game_sound(&game, H2_FRUIT_SLICE_SOUND_CARD));
  CHECK(!h2_fruit_slice_game_sound(&game, H2_FRUIT_SLICE_SOUND_CARD));
  CHECK(game.sound_count == H2_FRUIT_SLICE_SOUND_QUEUE);
  h2_fruit_slice_game_init(&game, 480, 480, 42, 0);
  h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_DOWN, 320, 20);
  CHECK(game.audio_mute == 0u); /* A drag out of settings never toggles. */
  h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_MOVE, 420, 288);
  h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_UP, 420, 288);
  CHECK(game.phase == H2_FRUIT_SLICE_PHASE_TITLE); /* Settings never slice. */
  CHECK(game.audio_mute == 0u);
  h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_DOWN, 320, 20);
  h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_UP, 320, 20);
  CHECK(game.audio_mute == H2_FRUIT_SLICE_MUTE_SFX);
}

static void tap_control(h2_fruit_slice_game_t* game, uint32_t button) {
  h2_fruit_slice_rect_t rect;
  CHECK(h2_fruit_slice_game_button_rect(game, button, &rect));
  const float x = (rect.x0 + rect.x1) * 0.5f;
  const float y = (rect.y0 + rect.y1) * 0.5f;
  h2_fruit_slice_game_pointer(game, H2_FRUIT_SLICE_POINTER_DOWN, x, y);
  h2_fruit_slice_game_pointer(game, H2_FRUIT_SLICE_POINTER_UP, x, y);
}

static void expect_sounds(h2_fruit_slice_game_t* game,
                          const h2_fruit_slice_sound_t* expected,
                          size_t count) {
  h2_fruit_slice_sound_t sound;
  for (size_t i = 0; i < count; ++i) {
    CHECK(h2_fruit_slice_game_take_sound(game, &sound));
    CHECK(sound == expected[i]);
  }
  CHECK(!h2_fruit_slice_game_take_sound(game, &sound));
}

static void test_economy_sound_transitions(void) {
  h2_fruit_slice_game_t game;
  h2_fruit_slice_game_init(&game, 480, 480, 42u, 0u);
  game.progress.fruit_credit = 10u;
  tap_control(&game, H2_FRUIT_SLICE_BUTTON_TASKS);
  expect_sounds(&game, NULL, 0u);
  tap_control(&game, H2_FRUIT_SLICE_BUTTON_CLAIM);
  const h2_fruit_slice_sound_t reward[] = {H2_FRUIT_SLICE_SOUND_TASK,
                                           H2_FRUIT_SLICE_SOUND_COIN};
  expect_sounds(&game, reward, 2u);
  CHECK(game.progress.coins == 15u && game.progress.fruit_credit == 0u);
  tap_control(&game, H2_FRUIT_SLICE_BUTTON_CLAIM);
  expect_sounds(&game, NULL, 0u); /* Consumed credit cannot celebrate again. */
  tap_control(&game, H2_FRUIT_SLICE_BUTTON_BACK);
  tap_control(&game, H2_FRUIT_SLICE_BUTTON_SHOP);
  tap_control(&game, H2_FRUIT_SLICE_BUTTON_WEAPON + 1u);
  expect_sounds(&game, NULL, 0u); /* Insufficient funds. */
  CHECK(game.progress.coins == 15u && game.progress.owned == 1u);

  game.progress.coins = 40u;
  h2_fruit_slice_rect_t rect;
  CHECK(h2_fruit_slice_game_button_rect(
      &game, H2_FRUIT_SLICE_BUTTON_WEAPON + 1u, &rect));
  h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_DOWN,
                              rect.x0 + 1.0f, rect.y0 + 1.0f);
  h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_CANCEL,
                              rect.x0 + 1.0f, rect.y0 + 1.0f);
  expect_sounds(&game, NULL, 0u);
  CHECK(game.progress.coins == 40u && game.progress.owned == 1u);
  tap_control(&game, H2_FRUIT_SLICE_BUTTON_WEAPON + 1u);
  const h2_fruit_slice_sound_t purchase[] = {H2_FRUIT_SLICE_SOUND_PURCHASE,
                                             H2_FRUIT_SLICE_SOUND_EQUIP};
  expect_sounds(&game, purchase, 2u);
  CHECK(game.progress.coins == 0u && game.progress.owned == 3u &&
        game.progress.equipped == 1u);
  tap_control(&game, H2_FRUIT_SLICE_BUTTON_WEAPON + 1u);
  expect_sounds(&game, NULL, 0u); /* Already equipped. */
  const h2_fruit_slice_sound_t equip[] = {H2_FRUIT_SLICE_SOUND_EQUIP};
  tap_control(&game, H2_FRUIT_SLICE_BUTTON_WEAPON);
  expect_sounds(&game, equip, 1u);
  tap_control(&game, H2_FRUIT_SLICE_BUTTON_WEAPON + 1u);
  expect_sounds(&game, equip, 1u); /* Owned equipment never purchases again. */
  CHECK(game.progress.coins == 0u && game.progress.owned == 3u &&
        game.progress.equipped == 1u);
}

static const h2_fruit_slice_sound_t k_weapon_hits[] = {
    H2_FRUIT_SLICE_SOUND_SLICE,       H2_FRUIT_SLICE_SOUND_SLICE_JADE,
    H2_FRUIT_SLICE_SOUND_SLICE_FLAME, H2_FRUIT_SLICE_SOUND_SLICE_BAMBOO,
    H2_FRUIT_SLICE_SOUND_SLICE_FROST, H2_FRUIT_SLICE_SOUND_SLICE_STORM,
};

static void swipe(h2_fruit_slice_game_t* game, float x0, float x1, float y) {
  h2_fruit_slice_game_pointer(game, H2_FRUIT_SLICE_POINTER_DOWN, x0, y);
  h2_fruit_slice_game_pointer(game, H2_FRUIT_SLICE_POINTER_MOVE, x1, y);
  h2_fruit_slice_game_pointer(game, H2_FRUIT_SLICE_POINTER_UP, x1, y);
}

static void test_weapon_hit_sounds(void) {
  for (unsigned id = 0; id < H2_FRUIT_SLICE_WEAPONS; ++id) {
    h2_fruit_slice_game_t game;
    h2_fruit_slice_game_init(&game, 480, 480, 42u, 0u);
    game.progress.coins = 120u;
    tap_control(&game, H2_FRUIT_SLICE_BUTTON_SHOP);
    const uint32_t button = H2_FRUIT_SLICE_BUTTON_WEAPON + id;
    h2_fruit_slice_rect_t rect;
    if (!h2_fruit_slice_game_button_rect(&game, button, &rect))
      tap_control(&game, H2_FRUIT_SLICE_BUTTON_NEXT);
    tap_control(&game, button);
    const h2_fruit_slice_sound_t bought[] = {H2_FRUIT_SLICE_SOUND_PURCHASE,
                                             H2_FRUIT_SLICE_SOUND_EQUIP};
    expect_sounds(&game, bought, id == H2_FRUIT_SLICE_WEAPON_CLASSIC ? 0u : 2u);
    CHECK(game.progress.equipped == id);
    if (id != H2_FRUIT_SLICE_WEAPON_CLASSIC) {
      if (!h2_fruit_slice_game_button_rect(&game, H2_FRUIT_SLICE_BUTTON_WEAPON,
                                           &rect))
        tap_control(&game, H2_FRUIT_SLICE_BUTTON_PREVIOUS);
      tap_control(&game, H2_FRUIT_SLICE_BUTTON_WEAPON);
      expect_sounds(&game, &bought[1], 1u);
      if (!h2_fruit_slice_game_button_rect(&game, button, &rect))
        tap_control(&game, H2_FRUIT_SLICE_BUTTON_NEXT);
      tap_control(&game, button);
      expect_sounds(&game, &bought[1], 1u);
      CHECK(game.progress.equipped == id);
    }
    tap_control(&game, H2_FRUIT_SLICE_BUTTON_BACK);
    tap_control(&game, H2_FRUIT_SLICE_BUTTON_START);
    CHECK(game.phase == H2_FRUIT_SLICE_PHASE_PLAYING);
    memset(game.fruits, 0, sizeof(game.fruits));
    const h2_fruit_slice_sound_t swing = H2_FRUIT_SLICE_SOUND_SWING;
    swipe(&game, 100, 380, 190);
    expect_sounds(&game, &swing, 1u);

    const h2_fruit_slice_fruit_t apple = {.active = true,
                                          .kind = H2_FRUIT_SLICE_APPLE,
                                          .radius = 32,
                                          .body = {.x = 240, .y = 250}};
    game.fruits[0] = apple;
    swipe(&game, 232, 248, 250); /* Above Frost's minimum, below SWING. */
    CHECK(game.score == 1u && !game.fruits[0].active);
    expect_sounds(&game, &k_weapon_hits[id], 1u);

    for (unsigned i = 0; i < 3u; ++i) {
      game.fruits[i] = apple;
      game.fruits[i].body.x = 150.0f + 90.0f * (float)i;
    }
    swipe(&game, 100, 380, 250);
    CHECK(game.score == 7u && game.sliced_total == 4u);
    const h2_fruit_slice_sound_t combo[] = {swing, k_weapon_hits[id],
                                            H2_FRUIT_SLICE_SOUND_COMBO,
                                            H2_FRUIT_SLICE_SOUND_CHEER};
    expect_sounds(&game, combo, sizeof(combo) / sizeof(combo[0]));

    game.fruits[0] = apple;
    game.fruits[0].kind = H2_FRUIT_SLICE_BOMB;
    swipe(&game, 232, 248, 250);
    CHECK(game.phase == H2_FRUIT_SLICE_PHASE_GAME_OVER);
    const h2_fruit_slice_sound_t bomb[] = {H2_FRUIT_SLICE_SOUND_FAIL,
                                           H2_FRUIT_SLICE_SOUND_BOMB};
    expect_sounds(&game, bomb, sizeof(bomb) / sizeof(bomb[0]));
  }
}

static void test_rapid_weapon_mix(void) {
  for (unsigned id = 0; id < H2_FRUIT_SLICE_WEAPONS; ++id) {
    fixture_t f = {0};
    h2_runtime_t rt = runtime_for(&f);
    h2_fruit_slice_audio_t audio;
    CHECK(h2_fruit_slice_audio_init(&audio, &rt) == H2_PAL_OK);
    h2_fruit_slice_audio_emit(&audio, k_weapon_hits[id], 100u);
    /* A queued hit from another equip still shares the 35 ms limit. */
    h2_fruit_slice_audio_emit(
        &audio, k_weapon_hits[(id + 1u) % H2_FRUIT_SLICE_WEAPONS], 134u);
    CHECK(active(&audio) == 1u);
    h2_fruit_slice_audio_emit(&audio, k_weapon_hits[id], 135u);
    CHECK(active(&audio) == 2u);
    CHECK(h2_fruit_slice_audio_suspend(&audio) == H2_PAL_OK);

    for (uint64_t now = 0u; now <= 4000u; now += 5u) {
      if (now % 35u == 0u)
        h2_fruit_slice_audio_emit(&audio, k_weapon_hits[id], now);
      if (now % 100u == 0u)
        h2_fruit_slice_audio_emit(&audio, H2_FRUIT_SLICE_SOUND_SWING, now);
      if (now % 230u == 200u) {
        h2_fruit_slice_audio_emit(&audio, H2_FRUIT_SLICE_SOUND_COMBO, now);
        h2_fruit_slice_audio_emit(&audio, H2_FRUIT_SLICE_SOUND_CHEER, now);
      }
      CHECK(h2_fruit_slice_audio_pump(&audio, now) == H2_PAL_OK);
    }
    printf("rapid weapon %u: S16 peak=%u, writes=%u\n", id, f.peak, f.writes);
    CHECK(f.peak > 5000u && f.peak < 32767u);
    CHECK(f.writes == 202u && f.opens == 1u && audio.rebases == 0u);
    CHECK(h2_fruit_slice_audio_close(&audio) == H2_PAL_OK);
  }
}

/* A consumer advances independently of the producer, unlike an always-ready
 * mock. A regular 30 ms render cadence must feed a 20 ms PCM device without
 * periodic holes; this failed on the original due-only scheduler. */
static void test_pcm_deadline_lookahead(void) {
  fixture_t f = {.paced_sink = true};
  h2_runtime_t rt = runtime_for(&f);
  h2_fruit_slice_audio_t audio;
  CHECK(h2_fruit_slice_audio_init(&audio, &rt) == H2_PAL_OK);
  for (unsigned i = 0u; i <= 100u; ++i) {
    f.now_ms = i * 30u;
    if (i % 3u == 0u)
      h2_fruit_slice_audio_emit(&audio, H2_FRUIT_SLICE_SOUND_SLICE, f.now_ms);
    CHECK(h2_fruit_slice_audio_pump(&audio, f.now_ms) == H2_PAL_OK);
  }
  printf("pcm simulated 3000ms: starvation=%u full=%u queue=%u\n", f.starved_ms,
         f.full_writes, f.queued_ms);
  CHECK(f.starved_ms == 0u && f.full_writes == 0u);
  CHECK(f.queued_ms <= 40u && audio.max_pump_gap_ms == 30u);
  CHECK(h2_fruit_slice_audio_close(&audio) == H2_PAL_OK);
}

static void test_silent_prepare_preserves_gesture_timeline(void) {
  fixture_t f = {0};
  h2_runtime_t rt = runtime_for(&f);
  h2_fruit_slice_audio_t audio;
  CHECK(h2_fruit_slice_audio_init(&audio, &rt) == H2_PAL_OK);
  CHECK(h2_fruit_slice_audio_prepare(&audio) == H2_PAL_OK);
  CHECK(h2_fruit_slice_audio_prepare(&audio) == H2_PAL_OK);
  CHECK(f.opens == 1u && f.writes == 0u && audio.music_position == 0u);
  CHECK(!audio.timeline_started);
  CHECK(h2_fruit_slice_audio_pump(&audio, 100000u) == H2_PAL_OK);
  CHECK(f.opens == 1u && f.writes == 2u && audio.rebases == 0u);
  CHECK(audio.started_ms == 100000u);
  CHECK(h2_fruit_slice_audio_set_mute(&audio, 3u) == H2_PAL_OK);
  CHECK(h2_fruit_slice_audio_pump(&audio, 100020u) == H2_PAL_OK);
  CHECK(h2_fruit_slice_audio_pump(&audio, 100040u) == H2_PAL_OK);
  CHECK(h2_fruit_slice_audio_prepare(&audio) == H2_PAL_OK);
  CHECK(f.opens == 1u && f.closes == 1u && !audio.speaker_started);
  CHECK(h2_fruit_slice_audio_close(&audio) == H2_PAL_OK);
}

static void test_long_frames_keep_bounded_lookahead(void) {
  /* The Web provider advertises 960/16000 = 60 ms. It must not enqueue
   * 120 ms of music on the first pump and delay a subsequent short SFX. */
  fixture_t f = {.format = {16000, 960, 1, H2_AUDIO_SAMPLE_S16LE}};
  h2_runtime_t rt = runtime_for(&f);
  h2_fruit_slice_audio_t audio;
  CHECK(h2_fruit_slice_audio_init(&audio, &rt) == H2_PAL_OK);
  CHECK(h2_fruit_slice_audio_pump(&audio, 0u) == H2_PAL_OK && f.writes == 1u);
  h2_fruit_slice_audio_emit(&audio, H2_FRUIT_SLICE_SOUND_SLICE, 30u);
  CHECK(h2_fruit_slice_audio_pump(&audio, 39u) == H2_PAL_OK && f.writes == 1u);
  CHECK(h2_fruit_slice_audio_pump(&audio, 40u) == H2_PAL_OK && f.writes == 2u);
  CHECK(h2_fruit_slice_audio_close(&audio) == H2_PAL_OK);
}

static void test_mute_gain_ramps(void) {
  const h2_audio_pcm_format_t formats[] = {
      {8000, 160, 1, H2_AUDIO_SAMPLE_S16LE},
      {16000, 320, 1, H2_AUDIO_SAMPLE_S16LE},
      {48000, 960, 2, H2_AUDIO_SAMPLE_S16LE},
      {16000, 960, 1, H2_AUDIO_SAMPLE_S16LE}};
  for (unsigned format = 0u; format < sizeof(formats) / sizeof(formats[0]);
       ++format) {
    unsigned hard_jump = 0u, ramp_jump = 0u;
    for (unsigned phase = 0u; phase < 32u; ++phase) {
      fixture_t f = {.format = formats[format], .preference = 0x41550101u};
      fixture_t reference = f;
      h2_runtime_t rt = runtime_for(&f), ref_rt = runtime_for(&reference);
      h2_fruit_slice_audio_t a, b;
      CHECK(h2_fruit_slice_audio_init(&a, &rt) == 0);
      CHECK(h2_fruit_slice_audio_init(&b, &ref_rt) == 0);
      a.music_position = b.music_position = (phase * 237u) * 65536u;
      CHECK(h2_fruit_slice_audio_prepare(&a) == 0);
      CHECK(h2_fruit_slice_audio_prepare(&b) == 0);
      CHECK(h2_fruit_slice_audio_set_mute(&a, 3u) == 0);
      CHECK(h2_fruit_slice_audio_pump(&a, 100u) == 0);
      CHECK(h2_fruit_slice_audio_pump(&b, 100u) == 0);
      const unsigned ramp_samples = formats[format].sample_rate_hz / 100u;
      for (unsigned i = 0u; i < formats[format].frame_samples_per_channel;
           ++i) {
        const int32_t gain =
            i + 1u < ramp_samples
                ? 65536 - (65536 / (int32_t)ramp_samples) * (int32_t)(i + 1u)
                : 0;
        for (unsigned c = 0u; c < formats[format].channels; ++c) {
          const unsigned at = i * formats[format].channels + c;
          CHECK(f.first_pcm[at] ==
                (int16_t)((int64_t)reference.first_pcm[at] * gain / 65536));
        }
      }
      const char* export_dir = getenv("H2_AUDIO_PCM_DIR");
      if (export_dir != NULL && phase == 7u) {
        char path[1024];
        CHECK(snprintf(path, sizeof(path), "%s/mute-%u-%u.s16le", export_dir,
                       formats[format].sample_rate_hz,
                       formats[format].frame_samples_per_channel) > 0);
        FILE* file = fopen(path, "wb");
        CHECK(file != NULL);
        for (unsigned i = 0u; i < formats[format].frame_samples_per_channel *
                                      formats[format].channels;
             ++i) {
          CHECK(fputc((uint16_t)f.first_pcm[i] & 255u, file) != EOF);
          CHECK(fputc(((uint16_t)f.first_pcm[i] >> 8) & 255u, file) != EOF);
        }
        CHECK(fclose(file) == 0);
      }
      const unsigned hard = (unsigned)abs(reference.first_pcm[0]);
      const unsigned ramp =
          (unsigned)abs(f.first_pcm[0] - reference.first_pcm[0]);
      if (hard > hard_jump) hard_jump = hard;
      if (ramp > ramp_jump) ramp_jump = ramp;
      CHECK(a.ramp_left == 0u && a.music_gain == 0 && a.sfx_gain == 0);
      CHECK(h2_fruit_slice_audio_pump(&a, 120u) == 0);
      CHECK(a.track == NULL && f.closes == 1u);
      CHECK(h2_fruit_slice_audio_set_mute(&a, H2_FRUIT_SLICE_MUTE_SFX) == 0);
      CHECK(h2_fruit_slice_audio_pump(&a, 140u) == 0);
      CHECK(a.music_gain == 65536 && a.track != NULL);
      CHECK(h2_fruit_slice_audio_close(&a) == 0);
      CHECK(h2_fruit_slice_audio_close(&b) == 0);
    }
    printf(
        "mute PCM rate=%u channels=%u: hard-switch delta=%u ramp-first delta=%u\n",
        formats[format].sample_rate_hz, formats[format].channels, hard_jump,
        ramp_jump);
    CHECK(hard_jump > 100u && ramp_jump < hard_jump / 20u);
  }
}

static void test_backpressure_commits_only_accepted_pcm(void) {
  const h2_audio_pcm_format_t formats[] = {
      {8000, 20, 1, H2_AUDIO_SAMPLE_S16LE},
      {16000, 40, 1, H2_AUDIO_SAMPLE_S16LE},
      {48000, 120, 2, H2_AUDIO_SAMPLE_S16LE},
      {16000, 40, 2, H2_AUDIO_SAMPLE_S16LE}};
  for (unsigned fm = 0u; fm < 4u; ++fm) {
    for (unsigned rejected_frame = 0u; rejected_frame < 4u; ++rejected_frame) {
      fixture_t f = {.format = formats[fm],
                     .preference = 0x41550101u,
                     .block_writes = true,
                     .accept_before_block = rejected_frame};
      h2_runtime_t rt = runtime_for(&f);
      h2_fruit_slice_audio_t a;
      CHECK(h2_fruit_slice_audio_init(&a, &rt) == 0);
      CHECK(h2_fruit_slice_audio_prepare(&a) == 0);
      CHECK(h2_fruit_slice_audio_set_mute(&a, 3u) == 0);
      uint64_t t = 100u;
      for (unsigned n = 0u; f.rejected == 0u && n < 4u; ++n, t += 5u)
        CHECK(h2_fruit_slice_audio_pump(&a, t) == 0);
      CHECK(f.rejected != 0u && f.accepted == rejected_frame);
      const uint32_t submitted =
          rejected_frame * formats[fm].frame_samples_per_channel;
      CHECK(a.music_position ==
            submitted * (8000u * 65536u / formats[fm].sample_rate_hz));
      CHECK(a.ramp_left == formats[fm].sample_rate_hz / 100u - submitted);
      const uint32_t position = a.music_position, remaining = a.ramp_left;
      const int32_t gain = a.music_gain;
      CHECK(h2_fruit_slice_audio_pump(&a, t) == 0);
      CHECK(a.music_position == position && a.ramp_left == remaining &&
            a.music_gain == gain && f.drains == 0u && f.closes == 0u);
      /* Reverse while still full. Neither a refused fade nor its reversal may
       * advance the audible music phase or commit a gain step. */
      CHECK(h2_fruit_slice_audio_set_mute(&a, H2_FRUIT_SLICE_MUTE_SFX) == 0);
      CHECK(h2_fruit_slice_audio_pump(&a, t + 5u) == 0);
      CHECK(a.music_position == position && a.music_gain == gain &&
            a.ramp_left == formats[fm].sample_rate_hz / 100u);
      f.block_writes = false;
      CHECK(h2_fruit_slice_audio_pump(&a, t + 10u) == 0);
      CHECK(a.music_position > position && a.music_gain >= gain);
      CHECK(f.opens == 1u && f.closes == 0u);
      CHECK(h2_fruit_slice_audio_close(&a) == 0);
    }
  }
}

static void test_muted_drain_reversal_and_timeout(void) {
  for (unsigned scenario = 0u; scenario < 3u; ++scenario) {
    fixture_t f = {.preference = 0x41550101u};
    h2_runtime_t rt = runtime_for(&f);
    h2_fruit_slice_audio_t a;
    CHECK(h2_fruit_slice_audio_init(&a, &rt) == 0);
    CHECK(h2_fruit_slice_audio_prepare(&a) == 0);
    CHECK(h2_fruit_slice_audio_set_mute(&a, 3u) == 0);
    if (scenario == 2u) {
      f.block_writes = true; /* No PCM is ever accepted. */
      CHECK(h2_fruit_slice_audio_pump(&a, 100u) == 0);
      CHECK(a.ramp_left == 160u && f.drains == 0u && f.closes == 0u);
      CHECK(h2_fruit_slice_audio_pump(&a, 100u + a.mute_timeout_ms) == 0);
      CHECK(f.accepted == 0u && f.drains == 0u && f.closes == 1u);
    } else {
      CHECK(h2_fruit_slice_audio_pump(&a, 100u) == 0 && f.accepted == 1u);
      f.drain_blocks = 1000u;
      CHECK(h2_fruit_slice_audio_pump(&a, 120u) == 0);
      CHECK(f.drains == 1u && f.closes == 0u && a.track != NULL);
      if (scenario == 0u) {
        CHECK(h2_fruit_slice_audio_set_mute(&a, H2_FRUIT_SLICE_MUTE_SFX) == 0);
        CHECK(h2_fruit_slice_audio_pump(&a, 140u) == 0);
        CHECK(f.opens == 1u && f.closes == 0u && a.music_gain == 65536);
      } else {
        CHECK(h2_fruit_slice_audio_pump(&a, 120u + a.mute_timeout_ms) == 0);
        CHECK(f.closes == 1u && f.stops == 1u && a.track == NULL);
      }
    }
    CHECK(h2_fruit_slice_audio_close(&a) == 0);
    CHECK(f.opens == f.closes && f.starts == f.stops);
  }
}

static void test_sfx_duck_release_boundary(void) {
  const h2_audio_pcm_format_t formats[] = {
      {8000, 160, 1, H2_AUDIO_SAMPLE_S16LE},
      {16000, 320, 1, H2_AUDIO_SAMPLE_S16LE},
      {48000, 960, 2, H2_AUDIO_SAMPLE_S16LE},
      {16000, 960, 1, H2_AUDIO_SAMPLE_S16LE}};
  for (unsigned fm = 0u; fm < 4u; ++fm) {
    for (unsigned phase = 0u; phase < 32u; ++phase) {
      fixture_t f = {.format = formats[fm]}, reference = f;
      reference.preference = 0x41550101u;
      h2_runtime_t rt = runtime_for(&f), rb = runtime_for(&reference);
      h2_fruit_slice_audio_t a, b;
      CHECK(h2_fruit_slice_audio_init(&a, &rt) == 0 &&
            h2_fruit_slice_audio_init(&b, &rb) == 0);
      a.music_position = b.music_position = phase * 37u * 65536u;
      h2_fruit_slice_audio_emit(&a, H2_FRUIT_SLICE_SOUND_BOMB, 1u);
      CHECK(h2_fruit_slice_audio_set_mute(&a, H2_FRUIT_SLICE_MUTE_SFX) == 0);
      CHECK(h2_fruit_slice_audio_pump(&a, 100u) == 0 &&
            h2_fruit_slice_audio_pump(&b, 100u) == 0);
      const unsigned boundary = formats[fm].sample_rate_hz / 100u - 1u;
      for (unsigned ch = 0u; ch < formats[fm].channels; ++ch) {
        const unsigned at = boundary * formats[fm].channels + ch;
        /* The last SFX sample is zero and BGM still carries its x10 level.
         * Allow one integer-PCM quantization step, never a second x10/24. */
        CHECK(abs(f.first_pcm[at] * 24 - reference.first_pcm[at] * 10) <= 24);
        const unsigned steady =
            (2u * (boundary + 1u) - 1u) * formats[fm].channels + ch;
        CHECK(f.first_pcm[steady] == reference.first_pcm[steady]);
      }
      CHECK(a.music_position == b.music_position && f.opens == 1u);
      CHECK(h2_fruit_slice_audio_close(&a) == 0 &&
            h2_fruit_slice_audio_close(&b) == 0);
    }
  }
}

static void test_pending_mute_suspend(void) {
  fixture_t f = {0};
  h2_runtime_t rt = runtime_for(&f);
  h2_fruit_slice_audio_t a;
  CHECK(h2_fruit_slice_audio_init(&a, &rt) == 0 &&
        h2_fruit_slice_audio_prepare(&a) == 0);
  CHECK(h2_fruit_slice_audio_set_mute(&a, 3u) == 0 && a.ramp_left != 0u);
  CHECK(h2_fruit_slice_audio_suspend(&a) == 0);
  CHECK(a.ramp_left == 0u && a.music_gain == 0 && a.sfx_gain == 0);
  CHECK(h2_fruit_slice_audio_pump(&a, 100u) == 0 && f.opens == 1u);
  CHECK(h2_fruit_slice_audio_set_mute(&a, 0u) == 0);
  CHECK(h2_fruit_slice_audio_pump(&a, 120u) == 0 && f.opens == 2u);
  CHECK(h2_fruit_slice_audio_close(&a) == 0);
}

int main(void) {
  test_backpressure_commits_only_accepted_pcm();
  test_muted_drain_reversal_and_timeout();
  test_sfx_duck_release_boundary();
  test_pending_mute_suspend();
  test_mute_gain_ramps();
  test_long_frames_keep_bounded_lookahead();
  test_silent_prepare_preserves_gesture_timeline();
  test_pcm_deadline_lookahead();
  test_economy_sound_transitions();
  test_weapon_hit_sounds();
  test_rapid_weapon_mix();
  test_bank_mute_and_lifecycle();
  test_bursts_backpressure_and_partial_failure();
  test_resampling_and_music_loop();
  test_sqlite_first_session_preferences();
  test_actual_game_transitions();
  puts("fruit slice audio tests passed");
  return 0;
}
