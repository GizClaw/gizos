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
  int create_result, write_result;
  uint32_t preference;
  uint32_t peak;
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
  ++f->writes;
  return f->write_result;
}
static int create(void* user, const h2_audio_track_config_t* config,
                  h2_pal_audio_track_t** out) {
  fixture_t* f = user;
  CHECK(config->buffer_frames == 2 && config->volume_factor_milli == 1000);
  if (f->create_result) return f->create_result;
  CHECK(f->active_tracks == 0);
  f->active_tracks = 1;
  ++f->opens;
  f->track = (h2_pal_audio_track_t){
      .user = f, .write = write_track, .close = close_track};
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
  CHECK(f.closes == 1 && f.stops == 1 && f.commits == 1);
  for (unsigned sound = 0; sound < H2_FRUIT_SLICE_SOUND_COUNT; ++sound) {
    CHECK(h2_fruit_slice_audio_suspend(&audio) == 0);
    f.peak = 0;
    h2_fruit_slice_audio_emit(&audio, (h2_fruit_slice_sound_t)sound,
                              1000 + sound * 1000);
    CHECK(h2_fruit_slice_audio_pump(&audio, 1000 + sound * 1000) == 0);
    CHECK(f.peak > 0); /* Every shipped event actually produces PCM. */
  }
  CHECK(h2_fruit_slice_audio_set_mute(&audio, 3) == 0);
  unsigned writes = f.writes;
  h2_fruit_slice_audio_emit(&audio, H2_FRUIT_SLICE_SOUND_BOMB, 50000);
  CHECK(active(&audio) == 0);
  CHECK(h2_fruit_slice_audio_pump(&audio, 50000) == 0 && f.writes == writes);
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
  unsigned writes = f.writes;
  CHECK(h2_fruit_slice_audio_pump(&audio, 100000) == 0);
  CHECK(f.writes == writes + 1); /* No long catch-up after a stall. */
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
  CHECK(f.writes == 501u && f.peak > 0 && f.peak < 4000u);
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

int main(void) {
  test_economy_sound_transitions();
  test_bank_mute_and_lifecycle();
  test_bursts_backpressure_and_partial_failure();
  test_resampling_and_music_loop();
  test_sqlite_first_session_preferences();
  test_actual_game_transitions();
  puts("fruit slice audio tests passed");
  return 0;
}
