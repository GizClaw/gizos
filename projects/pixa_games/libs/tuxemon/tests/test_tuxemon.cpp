#include "h2_tuxemon.h"

#include "../src/generated/tuxemon_assets.hpp"
#include "core/Scene.h"
#include "pixa.h"

#include <cassert>
#include <cstring>
#include <cstdio>
#include <memory>

namespace {

void sendButton(h2_tuxemon_t* game, h2_game_input_type_t type, uint8_t button) {
    const h2_game_input_event_t event = {
        type,
        0,
        0,
        button,
    };
    h2_tuxemon_handle_input(game, &event);
}

void move(h2_tuxemon_t* game, uint8_t button) {
    sendButton(game, H2_GAME_INPUT_BUTTON_DOWN, button);
    sendButton(game, H2_GAME_INPUT_BUTTON_UP, button);
    static_cast<pixelroot32::core::Scene*>(h2_tuxemon_scene(game))->update(260);
}

void movePath(h2_tuxemon_t* game, const char* path) {
    for (const char* step = path; *step != '\0'; ++step) {
        switch (*step) {
        case 'U':
            move(game, H2_TUXEMON_BUTTON_UP);
            break;
        case 'D':
            move(game, H2_TUXEMON_BUTTON_DOWN);
            break;
        case 'L':
            move(game, H2_TUXEMON_BUTTON_LEFT);
            break;
        case 'R':
            move(game, H2_TUXEMON_BUTTON_RIGHT);
            break;
        default:
            assert(false);
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    namespace assets = h2::tuxemon::assets;
    assert(assets::kRoomCount == 11);
    assert(assets::kPortalCount >= 20);
    assert(assets::kRooms[assets::kWorldRoomIndex].isWorld);
    assert(assets::kRooms[assets::kWorldRoomIndex].width == 64);
    assert(assets::kRooms[assets::kWorldRoomIndex].height == 60);
    const assets::Room& world = assets::kRooms[assets::kWorldRoomIndex];
    assert(world.traversal[18 * world.width + 22] ==
        (assets::kDirectionDown << 4u | assets::kDirectionUp));
    assert(world.traversal[20 * world.width + 60] == 0);
    assert(world.traversal[57 * world.width + 38] == 0);

    assert(argc == 2);
    std::FILE* file = std::fopen(argv[1], "rb");
    assert(file != nullptr);
    assert(std::fseek(file, 0, SEEK_END) == 0);
    const long fileSize = std::ftell(file);
    assert(fileSize > 0 && std::fseek(file, 0, SEEK_SET) == 0);
    std::unique_ptr<uint8_t[]> playerData(new uint8_t[static_cast<size_t>(fileSize)]);
    assert(std::fread(playerData.get(), static_cast<size_t>(fileSize), 1, file) == 1);
    assert(std::fclose(file) == 0);
    pixa_asset_t playerAsset{};
    assert(pixa_open_memory(playerData.get(), static_cast<size_t>(fileSize), &playerAsset) == PIXA_OK);

    h2_tuxemon_t* game = nullptr;
    const h2_tuxemon_config_t config = {&playerAsset};
    pixa_asset_t oversizedPlayerAsset = playerAsset;
    oversizedPlayerAsset.canvas.width = 129;
    const h2_tuxemon_config_t oversizedConfig = {&oversizedPlayerAsset};
    assert(h2_tuxemon_create(nullptr, &game) == H2_TUXEMON_ERR_INVALID_ARG);
    assert(h2_tuxemon_create(&config, nullptr) == H2_TUXEMON_ERR_INVALID_ARG);
    assert(h2_tuxemon_create(&oversizedConfig, &game) == H2_TUXEMON_ERR_ASSET);
    assert(game == nullptr);
    game = reinterpret_cast<h2_tuxemon_t*>(1);
    assert(h2_tuxemon_create(nullptr, &game) == H2_TUXEMON_ERR_INVALID_ARG);
    assert(game == nullptr);

    std::unique_ptr<uint8_t[]> malformedPlayerData(
        new uint8_t[static_cast<size_t>(fileSize)]);
    std::memcpy(
        malformedPlayerData.get(),
        playerData.get(),
        static_cast<size_t>(fileSize));
    pixa_asset_t malformedPlayerAsset{};
    assert(pixa_open_memory(
        malformedPlayerData.get(),
        static_cast<size_t>(fileSize),
        &malformedPlayerAsset) == PIXA_OK);
    pixa_clip_t malformedClip{};
    assert(pixa_find_clip(
        &malformedPlayerAsset,
        "run_left",
        &malformedClip) == PIXA_OK);
    constexpr size_t kFrameEntrySize = 16;
    malformedPlayerData[malformedPlayerAsset.frame_offset +
        static_cast<size_t>(malformedClip.first_frame) * kFrameEntrySize + 2] = 0xff;
    const h2_tuxemon_config_t malformedConfig = {&malformedPlayerAsset};
    assert(h2_tuxemon_create(&malformedConfig, &game) == H2_TUXEMON_ERR_ASSET);
    assert(game == nullptr);

    assert(h2_tuxemon_create(&config, &game) == H2_TUXEMON_OK);
    assert(game != nullptr);
    auto* scene = static_cast<pixelroot32::core::Scene*>(h2_tuxemon_scene(game));
    scene->init();

    h2_tuxemon_state_t state{};
    assert(h2_tuxemon_get_state(game, &state) == H2_TUXEMON_OK);
    assert(state.room == 0 && state.tile_x == 43 && state.tile_y == 46);
    assert(state.is_world == 1);
    assert(state.viewport_tile_x == 36 && state.viewport_tile_y == 39);

    sendButton(game, H2_GAME_INPUT_BUTTON_DOWN, H2_TUXEMON_BUTTON_UP);
    scene->update(260);
    scene->update(260);
    sendButton(game, H2_GAME_INPUT_BUTTON_UP, H2_TUXEMON_BUTTON_UP);
    scene->update(260);
    assert(h2_tuxemon_get_state(game, &state) == H2_TUXEMON_OK);
    assert(state.room == 1 && state.tile_x == 4 && state.tile_y == 4);
    assert(state.moving == 0);
    assert(state.is_world == 0);

    move(game, H2_TUXEMON_BUTTON_UP);
    move(game, H2_TUXEMON_BUTTON_LEFT);
    move(game, H2_TUXEMON_BUTTON_LEFT);
    move(game, H2_TUXEMON_BUTTON_LEFT);
    move(game, H2_TUXEMON_BUTTON_LEFT);
    move(game, H2_TUXEMON_BUTTON_UP);
    move(game, H2_TUXEMON_BUTTON_UP);
    assert(h2_tuxemon_get_state(game, &state) == H2_TUXEMON_OK);
    assert(state.room == 2 && state.tile_x == 8 && state.tile_y == 2);
    assert(state.room_change_count == 2);

    move(game, H2_TUXEMON_BUTTON_LEFT);
    move(game, H2_TUXEMON_BUTTON_RIGHT);
    assert(h2_tuxemon_get_state(game, &state) == H2_TUXEMON_OK);
    assert(state.room == 1 && state.tile_x == 0 && state.tile_y == 1);
    assert(state.room_change_count == 3);

    move(game, H2_TUXEMON_BUTTON_DOWN);
    move(game, H2_TUXEMON_BUTTON_DOWN);
    move(game, H2_TUXEMON_BUTTON_RIGHT);
    move(game, H2_TUXEMON_BUTTON_RIGHT);
    move(game, H2_TUXEMON_BUTTON_RIGHT);
    move(game, H2_TUXEMON_BUTTON_RIGHT);
    move(game, H2_TUXEMON_BUTTON_DOWN);
    move(game, H2_TUXEMON_BUTTON_DOWN);
    move(game, H2_TUXEMON_BUTTON_DOWN);
    assert(h2_tuxemon_get_state(game, &state) == H2_TUXEMON_OK);
    assert(state.room == 0 && state.tile_x == 43 && state.tile_y == 46);
    assert(state.is_world == 1);
    assert(state.room_change_count == 4);
    assert(state.viewport_tile_x == 36 && state.viewport_tile_y == 39);

    for (int step = 0; step < 3; ++step) {
        move(game, H2_TUXEMON_BUTTON_LEFT);
    }
    assert(h2_tuxemon_get_state(game, &state) == H2_TUXEMON_OK);
    assert(state.tile_x == 40 && state.viewport_tile_x == 36);
    sendButton(game, H2_GAME_INPUT_BUTTON_DOWN, H2_TUXEMON_BUTTON_LEFT);
    sendButton(game, H2_GAME_INPUT_BUTTON_UP, H2_TUXEMON_BUTTON_LEFT);
    scene->update(130);
    assert(h2_tuxemon_get_state(game, &state) == H2_TUXEMON_OK);
    assert(state.tile_x == 40 && state.moving == 1 && state.viewport_tile_x == 35);
    scene->update(130);
    assert(h2_tuxemon_get_state(game, &state) == H2_TUXEMON_OK);
    assert(state.tile_x == 39 && state.moving == 0 && state.viewport_tile_x == 35);
    for (int step = 0; step < 4; ++step) {
        move(game, H2_TUXEMON_BUTTON_RIGHT);
    }
    assert(h2_tuxemon_get_state(game, &state) == H2_TUXEMON_OK);
    assert(state.tile_x == 43 && state.viewport_tile_x == 35);

    movePath(game, "LLLLL");
    for (int step = 0; step < 21; ++step) {
        move(game, H2_TUXEMON_BUTTON_UP);
    }
    movePath(game, "LLLLLLLUULL");
    assert(h2_tuxemon_get_state(game, &state) == H2_TUXEMON_OK);
    assert(state.tile_x == 29 && state.tile_y == 25);

    sendButton(game, H2_GAME_INPUT_BUTTON_DOWN, H2_TUXEMON_BUTTON_DOWN);
    sendButton(game, H2_GAME_INPUT_BUTTON_UP, H2_TUXEMON_BUTTON_DOWN);
    scene->update(210);
    assert(h2_tuxemon_get_state(game, &state) == H2_TUXEMON_OK);
    assert(state.tile_x == 29 && state.tile_y == 25 && state.moving == 1);
    scene->update(210);
    assert(h2_tuxemon_get_state(game, &state) == H2_TUXEMON_OK);
    assert(state.tile_x == 29 && state.tile_y == 27 && state.moving == 0);

    move(game, H2_TUXEMON_BUTTON_UP);
    assert(h2_tuxemon_get_state(game, &state) == H2_TUXEMON_OK);
    assert(state.tile_x == 29 && state.tile_y == 27 && state.moving == 0);

    h2_tuxemon_destroy(game);

    h2_tuxemon_t* waterGame = nullptr;
    assert(h2_tuxemon_create(&config, &waterGame) == H2_TUXEMON_OK);
    auto* waterScene = static_cast<pixelroot32::core::Scene*>(
        h2_tuxemon_scene(waterGame));
    waterScene->init();
    movePath(waterGame, "DDDLLLLDDLDDDDDD");
    assert(h2_tuxemon_get_state(waterGame, &state) == H2_TUXEMON_OK);
    assert(state.room == 0 && state.tile_x == 38 && state.tile_y == 56);
    assert(state.moving == 0);
    h2_tuxemon_destroy(waterGame);

    h2_tuxemon_t* tallRoomGame = nullptr;
    assert(h2_tuxemon_create(&config, &tallRoomGame) == H2_TUXEMON_OK);
    auto* tallRoomScene = static_cast<pixelroot32::core::Scene*>(
        h2_tuxemon_scene(tallRoomGame));
    tallRoomScene->init();
    movePath(tallRoomGame, "DDDLLLLDDLDDRRRRRRU");
    assert(h2_tuxemon_get_state(tallRoomGame, &state) == H2_TUXEMON_OK);
    assert(state.room == 6 && state.tile_x == 6 && state.tile_y == 17);
    assert(state.is_world == 0 && state.viewport_tile_y == 3);
    h2_tuxemon_destroy(tallRoomGame);
    return 0;
}
