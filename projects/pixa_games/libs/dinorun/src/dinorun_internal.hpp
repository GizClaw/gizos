#pragma once

#include "core/Scene.h"
#include "h2_dinorun.h"

#include <array>
#include <cstdint>
#include <memory>

class DinoRunScene final : public pixelroot32::core::Scene {
public:
  explicit DinoRunScene(h2_dinorun *owner) : owner(owner) {}
  void init() override;
  void update(unsigned long deltaMs) override;
  void draw(pixelroot32::graphics::Renderer &renderer) override;

private:
  void stepFrame();
  h2_dinorun *owner;
};

struct h2_dinorun {
  explicit h2_dinorun(const h2_dinorun_config_t &value)
      : config(value), scene(this) {}

  h2_dinorun_config_t config;
  DinoRunScene scene;
  uint32_t initialSeed = 1;
  uint32_t randomState = 1;
  uint64_t accumulatorUs = 0;
  uint64_t elapsedUs = 0;
  uint64_t clipStartUs = 0;
  uint64_t gameOverStartedUs = 0;
  int32_t playerY = 152;
  int32_t playerYSubpixel = 152 * 16;
  int32_t velocityY = 0;
  int32_t obstacleX = 300;
  int32_t obstacleKind = 0;
  int32_t distance = 0;
  int32_t groundOffset = 0;
  int32_t speed = 7;
  int32_t chargeFrames = 0;
  bool actionDown = false;
  bool onGround = true;
  bool onBlock = false;
  bool overPit = false;
  bool started = false;
  bool gameOver = false;
  std::array<char, 64> gameOverStatus{};
  size_t gameOverStatusLen = 0;
  const char *clipName = nullptr;
  uint32_t decodedFrame = UINT32_MAX;
  const char *decodedClip = nullptr;
  std::unique_ptr<uint8_t[]> playerBgra;
  std::unique_ptr<uint8_t[]> playerArgb4444;
};

extern const h2_game_audio_recipe_t h2_dinorun_jump_recipe;
extern const h2_game_audio_recipe_t h2_dinorun_game_over_recipe;
