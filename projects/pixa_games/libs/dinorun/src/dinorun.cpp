#include "dinorun_internal.hpp"

#include "graphics/Color.h"
#include "graphics/Renderer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

namespace {

constexpr int kScreenWidth = 240;
constexpr int kScreenHeight = 240;
constexpr int kGroundY = 210;
constexpr int kPlayerX = 60;
constexpr int kPlayerWidth = 60;
constexpr int kPlayerHeight = 60;
constexpr int kPlayerStandingY = 152;
constexpr int kPlayerHitboxLeft = 76;
constexpr int kPlayerHitboxRight = 104;
constexpr int kPlayerHitboxTopOffset = 16;
constexpr int kPlayerHitboxBottomOffset = 52;
constexpr int kBlockSize = 32;
constexpr int kSpikeWidth = 40;
constexpr int kSpikeHeight = 11;
constexpr int kPitWidth = 60;
constexpr int kFrameUs = 22000;
constexpr int kSafeFrames = 90;
constexpr int kChargeLevelTwo = 6;
constexpr int kChargeLevelThree = 12;
constexpr int kChargeMax = 15;
constexpr uint64_t kGameOverOverlaySlideUs = 300000u;
constexpr int kPhysicsScale = 16;
constexpr int kGravityRising = 12;
constexpr int kGravityFalling = 16;
constexpr int kTerminalVelocity = 12 * kPhysicsScale;
constexpr int kBaseSpeed = 4;
constexpr int kMaxSpeed = 6;
constexpr uint16_t kBackgroundColor = 0x0841;

const h2_dinorun_player_clips_t kDefaultPlayerClips = {
    "run_right",
    "jump",
    "game_over",
};

const h2_dinorun_texts_t kEnglishTexts = {
    H2_GAME_TEXT_LITERAL("PRESS RECORD"),
    H2_GAME_TEXT_LITERAL("GAME OVER"),
    H2_GAME_TEXT_LITERAL("m"),
};

bool validText(h2_game_text_span_t text) {
  return text.data != nullptr && text.byte_len != 0u &&
         h2_game_text_validate_utf8(text) == H2_GAME_TEXT_OK;
}

h2_game_text_surface_t textSurface(pixelroot32::graphics::Renderer &renderer) {
  return {
      renderer.getDrawSurface().getPixelBuffer(),
      kScreenWidth,
      kScreenHeight,
      kScreenWidth,
      static_cast<size_t>(kScreenWidth * kScreenHeight),
  };
}

int measureText(const h2_dinorun *game, h2_game_text_span_t text,
                uint16_t lineHeight) {
  h2_game_text_metrics_t metrics{};
  return h2_game_text_measure(game->config.text, text, lineHeight, &metrics) ==
                 H2_GAME_TEXT_OK
             ? metrics.width_px
             : -1;
}

int textAdvance(const h2_dinorun *game, h2_game_text_span_t text,
                uint16_t lineHeight) {
  h2_game_text_metrics_t metrics{};
  return h2_game_text_measure(game->config.text, text, lineHeight, &metrics) ==
                 H2_GAME_TEXT_OK
             ? metrics.advance_px
             : -1;
}

void drawText(const h2_dinorun *game, pixelroot32::graphics::Renderer &renderer,
              h2_game_text_span_t text, int x, int y, uint16_t lineHeight) {
  const h2_game_text_surface_t surface = textSurface(renderer);
  const uint16_t color = pixelroot32::graphics::resolveColor(
      pixelroot32::graphics::Color::White,
      pixelroot32::graphics::PaletteContext::Sprite);
  (void)h2_game_text_draw(game->config.text, &surface, text, x, y,
                          {color, lineHeight});
}

void drawCenteredText(const h2_dinorun *game,
                      pixelroot32::graphics::Renderer &renderer,
                      h2_game_text_span_t text, int y, uint16_t lineHeight) {
  const int width = measureText(game, text, lineHeight);
  if (width >= 0)
    drawText(game, renderer, text, (kScreenWidth - width) / 2, y, lineHeight);
}

constexpr uint8_t kGroundArgb4444[] = {
#include "dinorun_ground_argb4444.inc"
};
constexpr uint8_t kBlockArgb4444[] = {
#include "dinorun_block_argb4444.inc"
};
constexpr uint8_t kSpikeArgb4444[] = {
#include "dinorun_spike_argb4444.inc"
};

static_assert(sizeof(kGroundArgb4444) == 60u * 30u * 2u);
static_assert(sizeof(kBlockArgb4444) == 32u * 32u * 2u);
static_assert(sizeof(kSpikeArgb4444) == 40u * 11u * 2u);

uint32_t nextRandom(h2_dinorun *game) {
  uint32_t value = game->randomState;
  value ^= value << 13;
  value ^= value >> 17;
  value ^= value << 5;
  game->randomState = value == 0 ? 0x6d2b79f5u : value;
  return game->randomState;
}

void emit(h2_dinorun *game, h2_dinorun_event_t event) {
  if (game->config.audio != nullptr) {
    const h2_game_audio_recipe_t *recipe = event == H2_DINORUN_EVENT_JUMP
                                               ? &h2_dinorun_jump_recipe
                                               : &h2_dinorun_game_over_recipe;
    const int rc = event == H2_DINORUN_EVENT_GAME_OVER
                       ? h2_game_audio_play_latest(game->config.audio, recipe)
                       : h2_game_audio_play(game->config.audio, recipe);
    (void)rc;
  }
  if (game->config.event_callback != nullptr) {
    game->config.event_callback(game->config.event_user, event);
  }
}

bool validClip(const pixa_asset_t *asset, const char *name) {
  pixa_clip_t clip = {};
  return pixa_find_clip(asset, name, &clip) == PIXA_OK &&
         clip.frame_count > 0;
}

const h2_dinorun_player_clips_t *playerClips(const h2_dinorun *game) {
  return game->config.player_clips != nullptr ? game->config.player_clips
                                              : &kDefaultPlayerClips;
}

const h2_dinorun_player_clips_t *
playerClips(const h2_dinorun_config_t *config) {
  return config->player_clips != nullptr ? config->player_clips
                                         : &kDefaultPlayerClips;
}

bool validPlayerClips(const pixa_asset_t *asset,
                      const h2_dinorun_player_clips_t *clips) {
  return clips != nullptr && clips->run_right != nullptr &&
         clips->jump != nullptr && clips->game_over != nullptr &&
         validClip(asset, clips->run_right) && validClip(asset, clips->jump) &&
         validClip(asset, clips->game_over);
}

void setClip(h2_dinorun *game, const char *name) {
  if (game->clipName == name)
    return;
  game->clipName = name;
  game->clipStartUs = game->elapsedUs;
  game->decodedFrame = UINT32_MAX;
  game->decodedClip = nullptr;
}

void blitArgb4444(pixelroot32::graphics::Renderer &renderer,
                  const uint8_t *pixels, uint16_t width, uint16_t height, int x,
                  int y) {
  (void)pixa_blit_argb4444_to_rgb565(
      renderer.getDrawSurface().getPixelBuffer(), kScreenWidth, kScreenHeight,
      kScreenWidth, pixels, width, height, static_cast<int16_t>(x),
      static_cast<int16_t>(y));
}

void drawPlayer(h2_dinorun *game, pixelroot32::graphics::Renderer &renderer) {
  pixa_clip_t clip = {};
  uint32_t frame = 0;
  if (pixa_find_clip(game->config.player, game->clipName, &clip) !=
          PIXA_OK ||
      pixa_frame_index_at_ms(game->config.player, &clip,
                                (game->elapsedUs - game->clipStartUs) / 1000u,
                                &frame) != PIXA_OK) {
    return;
  }
  if (game->decodedClip != game->clipName || game->decodedFrame != frame) {
    const size_t bgraSize =
        pixa_canvas_bgra_bytes(game->config.player->canvas);
    const size_t argbSize =
        pixa_canvas_argb4444_bytes(game->config.player->canvas);
    if (pixa_decode_clip_frame_bgra(game->config.player, game->clipName,
                                       frame, game->playerBgra.get(),
                                       bgraSize) != PIXA_OK ||
        pixa_bgra_to_argb4444(game->playerBgra.get(), bgraSize,
                                 game->playerArgb4444.get(),
                                 argbSize) != PIXA_OK) {
      return;
    }
    game->decodedClip = game->clipName;
    game->decodedFrame = frame;
  }
  const uint16_t sourceWidth = game->config.player->canvas.width;
  const uint16_t sourceHeight = game->config.player->canvas.height;
  uint16_t drawWidth = kPlayerWidth;
  uint16_t drawHeight = kPlayerHeight;
  if ((uint32_t)sourceWidth * kPlayerHeight >
      (uint32_t)sourceHeight * kPlayerWidth) {
    drawHeight = static_cast<uint16_t>(std::max<uint32_t>(
        1u, (uint32_t)sourceHeight * kPlayerWidth / sourceWidth));
  } else {
    drawWidth = static_cast<uint16_t>(std::max<uint32_t>(
        1u, (uint32_t)sourceWidth * kPlayerHeight / sourceHeight));
  }
  const int drawX = kPlayerX + (kPlayerWidth - drawWidth) / 2;
  const int drawY = game->playerY + kPlayerHeight - drawHeight;
  (void)pixa_blit_argb4444_scaled_to_rgb565(
      renderer.getDrawSurface().getPixelBuffer(), kScreenWidth, kScreenHeight,
      kScreenWidth, game->playerArgb4444.get(), sourceWidth, sourceHeight,
      static_cast<int16_t>(drawX), static_cast<int16_t>(drawY), drawWidth,
      drawHeight);
}

void resetGame(h2_dinorun *game) {
  game->randomState = game->initialSeed;
  game->accumulatorUs = 0;
  game->elapsedUs = 0;
  game->clipStartUs = 0;
  game->gameOverStartedUs = 0;
  game->playerY = kPlayerStandingY;
  game->playerYSubpixel = kPlayerStandingY * kPhysicsScale;
  game->velocityY = 0;
  game->obstacleX = kScreenWidth + 80;
  game->obstacleKind = 1 + static_cast<int>(nextRandom(game) % 3u);
  game->distance = 0;
  game->groundOffset = 0;
  game->speed = kBaseSpeed;
  game->chargeFrames = 0;
  game->actionDown = false;
  game->onGround = true;
  game->onBlock = false;
  game->overPit = false;
  game->started = false;
  game->gameOver = false;
  game->gameOverStatus[0] = '\0';
  game->gameOverStatusLen = 0u;
  game->clipName = playerClips(game)->run_right;
  game->decodedFrame = UINT32_MAX;
  game->decodedClip = nullptr;
}

void finishGame(h2_dinorun *game) {
  if (game->gameOver) {
    return;
  }
  game->gameOver = true;
  game->gameOverStartedUs = game->elapsedUs;
  game->actionDown = false;
  setClip(game, playerClips(game)->game_over);
  emit(game, H2_DINORUN_EVENT_GAME_OVER);
}

bool overlapsObstacle(const h2_dinorun *game) {
  const int playerTop = game->playerY + kPlayerHitboxTopOffset;
  const int playerBottom = game->playerY + kPlayerHitboxBottomOffset;
  if (game->obstacleKind == 1) {
    return kPlayerHitboxRight > game->obstacleX &&
           kPlayerHitboxLeft < game->obstacleX + kBlockSize &&
           playerBottom > kGroundY - kBlockSize && playerTop < kGroundY;
  }
  if (game->obstacleKind == 2) {
    return kPlayerHitboxRight > game->obstacleX &&
           kPlayerHitboxLeft < game->obstacleX + kSpikeWidth &&
           playerBottom > kGroundY - kSpikeHeight;
  }
  return false;
}

} // namespace

const h2_dinorun_texts_t *h2_dinorun_english_texts(void) {
  return &kEnglishTexts;
}

void DinoRunScene::init() {
  pixelroot32::core::Scene::init();
  resetGame(owner);
}

void DinoRunScene::update(unsigned long deltaMs) {
  if (!owner->started) {
    return;
  }
  if (owner->gameOver) {
    owner->elapsedUs += static_cast<uint64_t>(deltaMs) * 1000u;
    return;
  }
  owner->accumulatorUs += static_cast<uint64_t>(deltaMs) * 1000u;
  uint8_t steps = 0;
  while (!owner->gameOver && owner->accumulatorUs >= kFrameUs && steps < 8u) {
    stepFrame();
    owner->accumulatorUs -= kFrameUs;
    ++steps;
  }
}

void DinoRunScene::stepFrame() {
  owner->elapsedUs += kFrameUs;
  owner->groundOffset = (owner->groundOffset + owner->speed) % 60;
  if (owner->actionDown && owner->onGround) {
    owner->chargeFrames =
        std::min<int32_t>(owner->chargeFrames + 1, kChargeMax);
  }

  const int frame = static_cast<int>(owner->elapsedUs / kFrameUs);
  if (frame >= kSafeFrames) {
    owner->obstacleX -= owner->speed;
    owner->distance += owner->speed;
    owner->speed =
        std::min<int32_t>(kBaseSpeed + owner->distance / 5600, kMaxSpeed);
    if (owner->obstacleX < -kPitWidth) {
      owner->obstacleX =
          kScreenWidth + 80 + static_cast<int>(nextRandom(owner) % 140u);
      owner->obstacleKind = 1 + static_cast<int>(nextRandom(owner) % 3u);
    }
  }

  const bool obstacleHorizontalOverlap =
      kPlayerHitboxRight > owner->obstacleX &&
      kPlayerHitboxLeft <
          owner->obstacleX +
              (owner->obstacleKind == 1 ? kBlockSize : kPitWidth);
  if (owner->onBlock &&
      (owner->obstacleKind != 1 || !obstacleHorizontalOverlap)) {
    owner->onBlock = false;
    owner->onGround = false;
  }
  owner->overPit = owner->obstacleKind == 3 && obstacleHorizontalOverlap;
  if (owner->overPit && owner->onGround && !owner->onBlock) {
    owner->onGround = false;
    setClip(owner, playerClips(owner)->jump);
  }

  if (!owner->onGround) {
    const int previousBottom = owner->playerY + kPlayerHitboxBottomOffset;
    owner->velocityY += owner->velocityY < 0 ? kGravityRising : kGravityFalling;
    owner->velocityY = std::min<int32_t>(owner->velocityY, kTerminalVelocity);
    owner->playerYSubpixel += owner->velocityY;
    owner->playerY = owner->playerYSubpixel / kPhysicsScale;
    const int blockTop = kGroundY - kBlockSize;
    const bool landsOnBlock =
        owner->obstacleKind == 1 && obstacleHorizontalOverlap &&
        owner->velocityY >= 0 && previousBottom <= blockTop &&
        owner->playerY + kPlayerHitboxBottomOffset >= blockTop;
    if (landsOnBlock) {
      owner->playerY = blockTop - kPlayerHitboxBottomOffset;
      owner->playerYSubpixel = owner->playerY * kPhysicsScale;
      owner->velocityY = 0;
      owner->onGround = true;
      owner->onBlock = true;
      setClip(owner, playerClips(owner)->run_right);
    } else if (!owner->overPit && owner->playerY >= kPlayerStandingY) {
      owner->playerY = kPlayerStandingY;
      owner->playerYSubpixel = owner->playerY * kPhysicsScale;
      owner->velocityY = 0;
      owner->onGround = true;
      owner->onBlock = false;
      setClip(owner, playerClips(owner)->run_right);
    } else if (owner->playerY > kScreenHeight) {
      finishGame(owner);
      return;
    }
  }

  if (overlapsObstacle(owner)) {
    finishGame(owner);
    return;
  }
}

void DinoRunScene::draw(pixelroot32::graphics::Renderer &renderer) {
  renderer.drawFilledRectangleW(0, 0, kScreenWidth, kScreenHeight,
                                kBackgroundColor);
  for (int x = -owner->groundOffset; x < 240; x += 60) {
    blitArgb4444(renderer, kGroundArgb4444, 60, 30, x, kGroundY);
  }
  if (owner->obstacleKind == 3) {
    renderer.drawFilledRectangleW(owner->obstacleX, kGroundY, kPitWidth,
                                  kScreenHeight - kGroundY, kBackgroundColor);
  }
  if (owner->obstacleKind == 1) {
    blitArgb4444(renderer, kBlockArgb4444, kBlockSize, kBlockSize,
                 owner->obstacleX, kGroundY - kBlockSize);
  } else if (owner->obstacleKind == 2) {
    blitArgb4444(renderer, kSpikeArgb4444, kSpikeWidth, kSpikeHeight,
                 owner->obstacleX, kGroundY - kSpikeHeight);
  }
  drawPlayer(owner, renderer);

  char distanceText[24];
  std::snprintf(distanceText, sizeof(distanceText), "%ld",
                static_cast<long>(owner->distance / 60));
  const h2_game_text_span_t distance{
      distanceText,
      std::strlen(distanceText),
  };
  drawText(owner, renderer, distance, 104, 12, 8);
  const int distanceAdvance = textAdvance(owner, distance, 8);
  if (distanceAdvance >= 0 && distanceAdvance <= INT32_MAX - 104) {
    drawText(owner, renderer, owner->config.texts->distance_unit,
             104 + distanceAdvance, 12, 8);
  }
  if (!owner->started) {
    drawCenteredText(owner, renderer, owner->config.texts->press_record, 90,
                     16);
  } else if (owner->gameOver) {
    const uint64_t overlayElapsed =
        owner->elapsedUs >= owner->gameOverStartedUs
            ? owner->elapsedUs - owner->gameOverStartedUs
            : 0u;
    const uint64_t clamped =
        std::min<uint64_t>(overlayElapsed, kGameOverOverlaySlideUs);
    const int overlayY =
        static_cast<int>((kScreenHeight * (kGameOverOverlaySlideUs - clamped)) /
                         kGameOverOverlaySlideUs);
    uint16_t *pixels = renderer.getDrawSurface().getPixelBuffer();
    if (pixels != nullptr) {
      for (int y = overlayY; y < kScreenHeight; ++y) {
        for (int x = 0; x < kScreenWidth; ++x) {
          const uint16_t source = pixels[y * kScreenWidth + x];
          const uint16_t red =
              static_cast<uint16_t>((((source >> 11u) & 0x1fu) * 105u) / 255u);
          const uint16_t green =
              static_cast<uint16_t>((((source >> 5u) & 0x3fu) * 105u) / 255u);
          const uint16_t blue =
              static_cast<uint16_t>(((source & 0x1fu) * 105u) / 255u);
          pixels[y * kScreenWidth + x] =
              static_cast<uint16_t>((red << 11u) | (green << 5u) | blue);
        }
      }
    } else {
      renderer.drawFilledRectangleW(0, overlayY, kScreenWidth,
                                    kScreenHeight - overlayY, 0x0000);
    }
    drawCenteredText(owner, renderer, owner->config.texts->game_over,
                     overlayY + 72, 16);

    char scoreText[24];
    std::snprintf(scoreText, sizeof(scoreText), "%ld",
                  static_cast<long>(owner->distance / 60));
    const h2_game_text_span_t score{scoreText, std::strlen(scoreText)};
    const int scoreWidth = measureText(owner, score, 14);
    const int unitWidth =
        measureText(owner, owner->config.texts->distance_unit, 14);
    if (scoreWidth >= 0 && unitWidth >= 0) {
      const int x = (kScreenWidth - scoreWidth - unitWidth) / 2;
      drawText(owner, renderer, score, x, overlayY + 112, 14);
      drawText(owner, renderer, owner->config.texts->distance_unit,
               x + scoreWidth, overlayY + 112, 14);
    }

    if (owner->gameOverStatusLen != 0u) {
      const h2_game_text_span_t status{owner->gameOverStatus.data(),
                                       owner->gameOverStatusLen};
      drawCenteredText(owner, renderer, status, overlayY + 148, 14);
    }
  } else if (owner->actionDown && owner->onGround) {
    const int width =
        std::max<int32_t>(3, owner->chargeFrames * 30 / kChargeMax);
    renderer.drawFilledRectangleW(
        kPlayerX + 15, owner->playerY - 6, width, 4,
        owner->chargeFrames >= kChargeLevelThree ? 0xf800 : 0xffc0);
  }
}

int h2_dinorun_create(const h2_dinorun_config_t *config,
                      h2_dinorun_t **outGame) {
  if (outGame == nullptr) {
    return H2_DINORUN_ERR_INVALID_ARG;
  }
  *outGame = nullptr;
  if (config == nullptr || config->player == nullptr ||
      config->text == nullptr || config->text->vtable == nullptr ||
      config->text->vtable->measure == nullptr ||
      config->text->vtable->draw == nullptr || config->texts == nullptr ||
      !validText(config->texts->press_record) ||
      !validText(config->texts->game_over) ||
      !validText(config->texts->distance_unit)) {
    return H2_DINORUN_ERR_INVALID_ARG;
  }
  if (!validPlayerClips(config->player, playerClips(config))) {
    return H2_DINORUN_ERR_ASSET;
  }

  auto *game = new (std::nothrow) h2_dinorun(*config);
  if (game == nullptr) {
    return H2_DINORUN_ERR_NO_MEMORY;
  }
  const size_t bgraSize = pixa_canvas_bgra_bytes(config->player->canvas);
  const size_t argbSize = pixa_canvas_argb4444_bytes(config->player->canvas);
  game->playerBgra.reset(new (std::nothrow) uint8_t[bgraSize]);
  game->playerArgb4444.reset(new (std::nothrow) uint8_t[argbSize]);
  if (!game->playerBgra || !game->playerArgb4444) {
    delete game;
    return H2_DINORUN_ERR_NO_MEMORY;
  }
  game->initialSeed = config->seed == 0 ? 1 : config->seed;
  game->scene.init();
  *outGame = game;
  return H2_DINORUN_OK;
}

h2_game_scene_t *h2_dinorun_scene(h2_dinorun_t *game) {
  return game == nullptr ? nullptr
                         : reinterpret_cast<h2_game_scene_t *>(&game->scene);
}

void h2_dinorun_handle_input(h2_dinorun_t *game,
                             const h2_game_input_event_t *event) {
  if (game == nullptr || event == nullptr ||
      event->button != H2_DINORUN_BUTTON_ACTION || game->gameOver) {
    return;
  }
  if (event->type == H2_GAME_INPUT_BUTTON_DOWN) {
    game->started = true;
    if (game->actionDown) {
      return;
    }
    game->actionDown = true;
    if (game->onGround) {
      game->chargeFrames = 0;
    }
    return;
  }
  if (event->type != H2_GAME_INPUT_BUTTON_UP) {
    return;
  }

  game->actionDown = false;
  if (!game->onGround) {
    if (game->velocityY < -4 * kPhysicsScale) {
      game->velocityY = -4 * kPhysicsScale;
    }
    return;
  }
  const int charge = game->chargeFrames;
  game->velocityY = charge >= kChargeLevelThree ? -240
                    : charge >= kChargeLevelTwo ? -200
                                                : -144;
  game->onGround = false;
  game->onBlock = false;
  setClip(game, playerClips(game)->jump);
  emit(game, H2_DINORUN_EVENT_JUMP);
}

void h2_dinorun_reset(h2_dinorun_t *game) {
  if (game != nullptr) {
    resetGame(game);
  }
}

int h2_dinorun_set_game_over_status(h2_dinorun_t *game,
                                    h2_game_text_span_t status) {
  if (game == nullptr || status.byte_len >= game->gameOverStatus.size() ||
      (status.byte_len != 0u &&
       (status.data == nullptr ||
        h2_game_text_validate_utf8(status) != H2_GAME_TEXT_OK))) {
    return H2_DINORUN_ERR_INVALID_ARG;
  }
  if (status.byte_len != 0u) {
    std::memcpy(game->gameOverStatus.data(), status.data, status.byte_len);
  }
  game->gameOverStatus[status.byte_len] = '\0';
  game->gameOverStatusLen = status.byte_len;
  return H2_DINORUN_OK;
}

int h2_dinorun_get_result(const h2_dinorun_t *game,
                          h2_dinorun_result_t *outResult) {
  if (game == nullptr || outResult == nullptr) {
    return H2_DINORUN_ERR_INVALID_ARG;
  }
  *outResult = {game->distance / 60, game->gameOver ? 1 : 0};
  return H2_DINORUN_OK;
}

void h2_dinorun_destroy(h2_dinorun_t *game) {
  if (game == nullptr) {
    return;
  }
  delete game;
}
