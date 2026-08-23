#include "h2_game_test_render.hpp"
#include "dinorun_internal.hpp"

#include <algorithm>
#include <cassert>
#include <fstream>
#include <iterator>
#include <vector>

struct Events {
  int jump = 0;
  int gameOver = 0;
};

constexpr uint32_t kGameplayFrameMs = 22;

static void onEvent(void *user, h2_dinorun_event_t event) {
  auto *events = static_cast<Events *>(user);
  if (event == H2_DINORUN_EVENT_JUMP) {
    ++events->jump;
  } else if (event == H2_DINORUN_EVENT_GAME_OVER) {
    ++events->gameOver;
  }
}

static std::vector<unsigned char> readFile(const char *path) {
  std::ifstream input(path, std::ios::binary);
  return {
      std::istreambuf_iterator<char>(input),
      std::istreambuf_iterator<char>(),
  };
}

static void action(h2_dinorun_t *game, h2_game_input_type_t type) {
  const h2_game_input_event_t event = {
      type,
      0,
      0,
      H2_DINORUN_BUTTON_ACTION,
  };
  h2_dinorun_handle_input(game, &event);
}

static int jumpHeight(h2_dinorun_t *game, int chargeFrames) {
  h2_dinorun_reset(game);
  action(game, H2_GAME_INPUT_BUTTON_DOWN);
  for (int i = 0; i < chargeFrames; ++i) {
    game->scene.update(kGameplayFrameMs);
  }
  action(game, H2_GAME_INPUT_BUTTON_UP);
  int minimumY = game->playerY;
  for (int i = 0; i < 80 && !game->onGround; ++i) {
    game->scene.update(kGameplayFrameMs);
    minimumY = std::min(minimumY, game->playerY);
  }
  assert(game->onGround);
  return 152 - minimumY;
}

int main(int argc, char **argv) {
  assert(argc == 2);
  const auto playerData = readFile(argv[1]);
  pixa_asset_t player = {};
  assert(pixa_open_memory(playerData.data(), playerData.size(), &player) ==
         PIXA_OK);
  for (const char *name : {"run_right", "jump", "game_over"}) {
    pixa_clip_t clip = {};
    assert(pixa_find_clip(&player, name, &clip) == PIXA_OK);
    assert(clip.frame_count > 0);
  }

  Events events;
  const h2_game_text_api_t text = h2_game_text_builtin_5x7();
  const h2_dinorun_config_t config = {
      &player,     nullptr, &text,   h2_dinorun_english_texts(),
      0x12345678u, onEvent, &events, nullptr,
  };
  h2_dinorun_t *game = nullptr;
  assert(h2_dinorun_create(&config, &game) == H2_DINORUN_OK);
  assert(game != nullptr && !game->started);

  const h2_dinorun_player_clips_t remappedClips = {
      "run_right",
      "run_right",
      "run_right",
  };
  h2_dinorun_config_t remappedConfig = config;
  remappedConfig.player_clips = &remappedClips;
  h2_dinorun_t *remappedGame = nullptr;
  assert(h2_dinorun_create(&remappedConfig, &remappedGame) == H2_DINORUN_OK);
  h2_dinorun_destroy(remappedGame);

  action(game, H2_GAME_INPUT_BUTTON_DOWN);
  for (int i = 0; i < 5; ++i) {
    game->scene.update(kGameplayFrameMs);
  }
  assert(game->chargeFrames == 5);
  const int groundOffset = game->groundOffset;
  assert(groundOffset > 0);
  action(game, H2_GAME_INPUT_BUTTON_DOWN);
  assert(game->chargeFrames == 5);
  game->scene.update(kGameplayFrameMs);
  assert(game->chargeFrames == 6 && game->groundOffset != groundOffset);
  action(game, H2_GAME_INPUT_BUTTON_UP);
  assert(events.jump == 1 && !game->onGround && game->velocityY < 0);
  assert(game->clipStartUs == game->elapsedUs);
  for (int i = 0; i < 80 && !game->onGround; ++i) {
    game->scene.update(kGameplayFrameMs);
  }
  assert(game->onGround);

  const int levelOneHeight = jumpHeight(game, 1);
  const int levelTwoHeight = jumpHeight(game, 6);
  const int levelThreeHeight = jumpHeight(game, 12);
  assert(levelOneHeight >= 48 && levelOneHeight <= 52);
  assert(levelTwoHeight >= 96 && levelTwoHeight <= 100);
  assert(levelThreeHeight >= 140 && levelThreeHeight <= 144);
  assert(levelOneHeight < levelTwoHeight && levelTwoHeight < levelThreeHeight);

  h2_dinorun_reset(game);
  game->started = true;
  game->obstacleKind = 1;
  game->obstacleX = 80;
  game->playerY = 100;
  game->playerYSubpixel = game->playerY * 16;
  game->velocityY = 10 * 16;
  game->onGround = false;
  for (int i = 0; i < 10 && !game->onBlock; ++i) {
    game->scene.update(kGameplayFrameMs);
  }
  assert(game->onGround && game->onBlock && !game->gameOver);

  h2_dinorun_reset(game);
  game->started = true;
  game->obstacleKind = 3;
  game->obstacleX = 80;
  for (int i = 0; i < 30 && !game->gameOver; ++i) {
    game->scene.update(kGameplayFrameMs);
  }
  assert(game->gameOver && events.gameOver == 1);
  const uint64_t gameOverElapsedUs = game->elapsedUs;
  game->scene.update(kGameplayFrameMs);
  assert(game->elapsedUs > gameOverElapsedUs);

  h2_dinorun_reset(game);
  game->started = true;
  game->obstacleKind = 2;
  game->obstacleX = 80;
  game->scene.update(kGameplayFrameMs);
  assert(events.gameOver == 2);
  h2_dinorun_result_t result = {};
  assert(h2_dinorun_get_result(game, &result) == H2_DINORUN_OK);
  assert(result.game_over == 1);
  const h2_game_text_span_t status = H2_GAME_TEXT_LITERAL("READY");
  assert(h2_dinorun_set_game_over_status(game, status) == H2_DINORUN_OK);
  assert(game->gameOverStatusLen == status.byte_len);
  assert(std::memcmp(game->gameOverStatus.data(), status.data,
                     status.byte_len) == 0);
  assert(h2_dinorun_set_game_over_status(nullptr, status) ==
         H2_DINORUN_ERR_INVALID_ARG);
  const uint8_t invalid_utf8[] = {0xffu};
  assert(h2_dinorun_set_game_over_status(
             game,
             h2_game_text_span_t{reinterpret_cast<const char *>(invalid_utf8),
                                 sizeof(invalid_utf8)}) ==
         H2_DINORUN_ERR_INVALID_ARG);
  char maximum_status[63];
  std::memset(maximum_status, 'x', sizeof(maximum_status));
  assert(h2_dinorun_set_game_over_status(
             game,
             h2_game_text_span_t{maximum_status, sizeof(maximum_status)}) ==
         H2_DINORUN_OK);
  char oversized_status[64];
  std::memset(oversized_status, 'x', sizeof(oversized_status));
  assert(h2_dinorun_set_game_over_status(
             game,
             h2_game_text_span_t{oversized_status,
                                 sizeof(oversized_status)}) ==
         H2_DINORUN_ERR_INVALID_ARG);
  assert(h2_dinorun_set_game_over_status(
             game, h2_game_text_span_t{nullptr, 0u}) == H2_DINORUN_OK);
  assert(game->gameOverStatusLen == 0u);

  h2_dinorun_reset(game);
  assert(!game->started && !game->gameOver && game->distance == 0 &&
         game->gameOverStatusLen == 0u);

  const h2_dinorun_texts_t translated = {
      H2_GAME_TEXT_LITERAL("按录音键开始"),
      H2_GAME_TEXT_LITERAL("游戏结束"),
      H2_GAME_TEXT_LITERAL("米"),
  };
  const h2_game_text_span_t expected[] = {
      translated.press_record, translated.game_over, translated.distance_unit};
  bool drawn[3]{};
  H2GameMockTextState text_state{expected, drawn, 3u};
  const h2_game_text_api_t translated_text = h2_game_test_text_api(&text_state);
  game->config.text = &translated_text;
  game->config.texts = &translated;
  auto renderer = h2_game_test_renderer();
  game->scene.draw(renderer);
  game->started = true;
  game->gameOver = true;
  game->scene.draw(renderer);
  assert(drawn[0] && drawn[1] && drawn[2]);
  h2_dinorun_destroy(game);

  h2_dinorun_config_t missingClipConfig = config;
  pixa_asset_t invalidPlayer = player;
  invalidPlayer.clip_count = 0;
  missingClipConfig.player = &invalidPlayer;
  game = reinterpret_cast<h2_dinorun_t *>(1);
  assert(h2_dinorun_create(&missingClipConfig, &game) == H2_DINORUN_ERR_ASSET);
  assert(game == nullptr);
  return 0;
}
