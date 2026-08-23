#include "h2_tuxemon.h"

#include "generated/tuxemon_assets.hpp"

#include "core/Actor.h"
#include "core/Scene.h"
#include "graphics/Camera2D.h"
#include "graphics/Color.h"
#include "graphics/Renderer.h"
#include "pixa.h"
#include "math/Scalar.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

namespace {

namespace assets = h2::tuxemon::assets;
namespace core = pixelroot32::core;
namespace gfx = pixelroot32::graphics;
namespace math = pixelroot32::math;

constexpr int kTileSize = 16;
constexpr int kViewportSize = 240;
constexpr int kViewportTiles = kViewportSize / kTileSize;
constexpr int kPlayerSpriteSize = 24;
constexpr uint16_t kMaxPlayerCanvasSize = 128;
constexpr uint32_t kMoveDurationMs = 260;
constexpr uint32_t kJumpDurationMs = 420;
constexpr int kJumpHeight = 8;
constexpr uint8_t kWorldSpawnX = 43;
constexpr uint8_t kWorldSpawnY = 46;
constexpr char kRunLeftClip[] = "run_left";
constexpr char kRunRightClip[] = "run_right";

class PlayerActor final : public core::Actor {
public:
    explicit PlayerActor(const pixa_asset_t* asset)
        : Actor(math::toScalar(0.0f), math::toScalar(0.0f), 12, 16), asset(asset) {
        if (asset == nullptr) {
            return;
        }
        sourceBgra.reset(new (std::nothrow) uint8_t[pixa_canvas_bgra_bytes(asset->canvas)]);
        scaledArgb4444.reset(new (std::nothrow) uint8_t[kPlayerSpriteSize * kPlayerSpriteSize * 2]);
    }

    bool ready() const {
        return sourceBgra != nullptr && scaledArgb4444 != nullptr;
    }

    void face(uint8_t button) {
        const char* nextClip = clipName;
        if (button == H2_TUXEMON_BUTTON_LEFT) {
            nextClip = kRunLeftClip;
        } else if (button == H2_TUXEMON_BUTTON_RIGHT) {
            nextClip = kRunRightClip;
        }
        if (nextClip != clipName) {
            clipName = nextClip;
            elapsedMs = 0;
            decodedFrame = UINT32_MAX;
        }
    }

    void setWalking(bool value) {
        if (!value && walking) {
            elapsedMs = 0;
            decodedFrame = UINT32_MAX;
        }
        walking = value;
    }

    void update(unsigned long deltaTime) override {
        if (walking) {
            elapsedMs += deltaTime;
        }
    }

    void draw(gfx::Renderer& renderer) override {
        pixa_clip_t clip{};
        if (!ready() || pixa_find_clip(asset, clipName, &clip) != PIXA_OK ||
            pixa_frame_index_at_ms(asset, &clip, elapsedMs, &frame) != PIXA_OK) {
            return;
        }
        if (decodedFrame != frame) {
            const size_t bgraSize = pixa_canvas_bgra_bytes(asset->canvas);
            if (pixa_decode_clip_frame_bgra(
                    asset,
                    clipName,
                    frame,
                    sourceBgra.get(),
                    bgraSize) != PIXA_OK) {
                return;
            }
            scaleCurrentFrame();
            decodedFrame = frame;
        }
        const int screenX = static_cast<int>(position.x) + renderer.getXOffset() -
            (kPlayerSpriteSize - width) / 2;
        const int screenY = static_cast<int>(position.y) + renderer.getYOffset() + height -
            kPlayerSpriteSize;
        (void)pixa_blit_argb4444_to_rgb565(
            renderer.getDrawSurface().getPixelBuffer(),
            kViewportSize,
            kViewportSize,
            kViewportSize,
            scaledArgb4444.get(),
            kPlayerSpriteSize,
            kPlayerSpriteSize,
            static_cast<int16_t>(screenX),
            static_cast<int16_t>(screenY));
    }

    core::Rect getHitBox() override {
        return {position, width, height};
    }

    void onCollision(core::Actor* other) override {
        (void)other;
    }

private:
    void scaleCurrentFrame() {
        for (int y = 0; y < kPlayerSpriteSize; ++y) {
            const int sourceY = y * asset->canvas.height / kPlayerSpriteSize;
            for (int x = 0; x < kPlayerSpriteSize; ++x) {
                const int sourceX = x * asset->canvas.width / kPlayerSpriteSize;
                const size_t sourceIndex =
                    (static_cast<size_t>(sourceY) * asset->canvas.width + sourceX) * 4u;
                const uint8_t* bgra = sourceBgra.get() + sourceIndex;
                const uint16_t argb4444 = static_cast<uint16_t>(
                    (static_cast<uint16_t>(bgra[3] >> 4u) << 12u) |
                    (static_cast<uint16_t>(bgra[2] >> 4u) << 8u) |
                    (static_cast<uint16_t>(bgra[1] >> 4u) << 4u) |
                    static_cast<uint16_t>(bgra[0] >> 4u));
                const size_t targetIndex = static_cast<size_t>(y * kPlayerSpriteSize + x) * 2u;
                scaledArgb4444[targetIndex] = static_cast<uint8_t>(argb4444);
                scaledArgb4444[targetIndex + 1u] = static_cast<uint8_t>(argb4444 >> 8u);
            }
        }
    }

    const pixa_asset_t* asset = nullptr;
    std::unique_ptr<uint8_t[]> sourceBgra;
    std::unique_ptr<uint8_t[]> scaledArgb4444;
    const char* clipName = kRunRightClip;
    uint64_t elapsedMs = 0;
    uint32_t frame = 0;
    uint32_t decodedFrame = UINT32_MAX;
    bool walking = false;
};

class TuxemonScene final : public core::Scene {
public:
    explicit TuxemonScene(const pixa_asset_t* playerAsset) : player(playerAsset) {}

    bool ready() const {
        return player.ready();
    }

    void init() override {
        Scene::init();
        gfx::enableDualPaletteMode(true);
        gfx::setBackgroundCustomPalette(assets::kBackgroundPalette);
        gfx::setSpritePalette(gfx::PaletteType::PR32);
        roomIndex = assets::kWorldRoomIndex;
        tileX = kWorldSpawnX;
        tileY = kWorldSpawnY;
        roomChangeCount = 0;
        portalLocked = false;
        moving = false;
        jumping = false;
        heldButtons = 0;
        activeButton = 0;
        configureRoom();
        addEntity(&player);
        syncPlayerPosition();
        centerCamera();
    }

    void update(unsigned long deltaTime) override {
        player.update(deltaTime);
        if (!moving) {
            startMove(activeButton);
            return;
        }
        moveElapsedMs += static_cast<uint32_t>(deltaTime);
        const uint32_t duration = movementDurationMs();
        if (moveElapsedMs >= duration) {
            tileX = targetTileX;
            tileY = targetTileY;
            moving = false;
            jumping = false;
            moveElapsedMs = duration;
        }
        syncPlayerPosition();
        updateCamera();
        if (!moving) {
            handlePortal();
            startMove(activeButton);
            if (!moving) {
                player.setWalking(false);
            }
        }
    }

    void draw(gfx::Renderer& renderer) override {
        if (roomUsesCamera(assets::kRooms[roomIndex])) {
            camera.apply(renderer);
        } else {
            renderer.setDisplayOffset(0, 0);
        }
        const uint8_t layerCount = assets::kRooms[roomIndex].layerCount;
        for (uint8_t index = 0; index + 1 < layerCount; ++index) {
            renderer.drawTileMap(tilemaps[index], originX, originY, gfx::LayerType::Dynamic);
        }
        player.draw(renderer);
        renderer.drawTileMap(tilemaps[layerCount - 1], originX, originY, gfx::LayerType::Dynamic);
    }

    void handleInput(const h2_game_input_event_t& event) {
        if (event.button < H2_TUXEMON_BUTTON_UP || event.button > H2_TUXEMON_BUTTON_RIGHT) {
            return;
        }
        const uint8_t buttonMask = static_cast<uint8_t>(1u << (event.button - 1u));
        if (event.type == H2_GAME_INPUT_BUTTON_UP) {
            heldButtons = static_cast<uint8_t>(heldButtons & ~buttonMask);
            if (activeButton == event.button) {
                activeButton = selectHeldButton();
            }
            return;
        }
        if (event.type != H2_GAME_INPUT_BUTTON_DOWN) {
            return;
        }
        heldButtons = static_cast<uint8_t>(heldButtons | buttonMask);
        activeButton = event.button;
        startMove(activeButton);
    }

    h2_tuxemon_state_t state() const {
        return {
            roomIndex,
            tileX,
            tileY,
            static_cast<uint8_t>(moving ? 1 : 0),
            static_cast<uint8_t>(assets::kRooms[roomIndex].isWorld ? 1 : 0),
            pageTileX,
            pageTileY,
            roomChangeCount,
        };
    }

protected:
    void resetState() noexcept override {
        Scene::resetState();
    }

private:
    void startMove(uint8_t button) {
        if (moving || button == 0) {
            return;
        }
        int dx = 0;
        int dy = 0;
        switch (button) {
        case H2_TUXEMON_BUTTON_UP:
            dy = -1;
            break;
        case H2_TUXEMON_BUTTON_DOWN:
            dy = 1;
            break;
        case H2_TUXEMON_BUTTON_LEFT:
            dx = -1;
            break;
        case H2_TUXEMON_BUTTON_RIGHT:
            dx = 1;
            break;
        default:
            return;
        }
        player.face(button);
        const int nextX = static_cast<int>(tileX) + dx;
        const int nextY = static_cast<int>(tileY) + dy;
        const uint8_t direction = directionForButton(button);
        if (!canTraverse(tileX, tileY, nextX, nextY, direction)) {
            return;
        }
        int targetX = nextX;
        int targetY = nextY;
        jumping = isDownwardLedge(nextX, nextY, direction);
        if (jumping) {
            targetY = nextY + 1;
            if (!canTraverse(nextX, nextY, targetX, targetY, assets::kDirectionDown)) {
                jumping = false;
                return;
            }
        }
        startTileX = tileX;
        startTileY = tileY;
        targetTileX = static_cast<uint8_t>(targetX);
        targetTileY = static_cast<uint8_t>(targetY);
        moveElapsedMs = 0;
        moving = true;
        player.setWalking(true);
    }

    uint8_t selectHeldButton() const {
        for (uint8_t button = H2_TUXEMON_BUTTON_RIGHT; button >= H2_TUXEMON_BUTTON_UP; --button) {
            if ((heldButtons & (1u << (button - 1u))) != 0) {
                return button;
            }
        }
        return 0;
    }

    void configureRoom() {
        const assets::Room& room = assets::kRooms[roomIndex];
        const int roomWidth = static_cast<int>(room.width) * kTileSize;
        const int roomHeight = static_cast<int>(room.height) * kTileSize;
        originX = roomWidth > kViewportSize ? 0 : (kViewportSize - roomWidth) / 2;
        originY = roomHeight > kViewportSize ? 0 : (kViewportSize - roomHeight) / 2;
        for (uint8_t index = 0; index < assets::kMaxLayers; ++index) {
            const assets::Layer& layer = room.layers[index];
            tilemaps[index] = {
                const_cast<uint8_t*>(layer.indices),
                room.width,
                room.height,
                layer.tiles,
                kTileSize,
                kTileSize,
                layer.tileCount,
                nullptr,
                nullptr,
                nullptr,
            };
        }
        camera.setBounds(
            math::toScalar(0),
            math::toScalar(roomWidth > kViewportSize ? roomWidth - kViewportSize : 0));
        camera.setVerticalBounds(
            math::toScalar(0),
            math::toScalar(roomHeight > kViewportSize ? roomHeight - kViewportSize : 0));
    }

    static bool roomUsesCamera(const assets::Room& room) {
        return room.isWorld || room.width > kViewportTiles || room.height > kViewportTiles;
    }

    static uint8_t directionForButton(uint8_t button) {
        switch (button) {
        case H2_TUXEMON_BUTTON_UP:
            return assets::kDirectionUp;
        case H2_TUXEMON_BUTTON_DOWN:
            return assets::kDirectionDown;
        case H2_TUXEMON_BUTTON_LEFT:
            return assets::kDirectionLeft;
        case H2_TUXEMON_BUTTON_RIGHT:
            return assets::kDirectionRight;
        default:
            return 0;
        }
    }

    static uint8_t oppositeDirection(uint8_t direction) {
        switch (direction) {
        case assets::kDirectionUp:
            return assets::kDirectionDown;
        case assets::kDirectionDown:
            return assets::kDirectionUp;
        case assets::kDirectionLeft:
            return assets::kDirectionRight;
        case assets::kDirectionRight:
            return assets::kDirectionLeft;
        default:
            return 0;
        }
    }

    bool canTraverse(int fromX, int fromY, int toX, int toY, uint8_t direction) const {
        const assets::Room& room = assets::kRooms[roomIndex];
        if (fromX < 0 || fromY < 0 || fromX >= room.width || fromY >= room.height ||
            toX < 0 || toY < 0 || toX >= room.width || toY >= room.height || direction == 0) {
            return false;
        }
        const uint8_t source = room.traversal[fromY * room.width + fromX];
        const uint8_t destination = room.traversal[toY * room.width + toX];
        return ((source >> 4u) & direction) != 0 &&
            (destination & oppositeDirection(direction)) != 0;
    }

    bool isDownwardLedge(int x, int y, uint8_t direction) const {
        if (direction != assets::kDirectionDown) {
            return false;
        }
        const assets::Room& room = assets::kRooms[roomIndex];
        const uint8_t traversal = room.traversal[y * room.width + x];
        return (traversal & 0x0Fu) == assets::kDirectionUp &&
            (traversal >> 4u) == assets::kDirectionDown;
    }

    uint32_t movementDurationMs() const {
        return jumping ? kJumpDurationMs : kMoveDurationMs;
    }

    void syncPlayerPosition() {
        int pixelX = static_cast<int>(tileX) * kTileSize;
        int pixelY = static_cast<int>(tileY) * kTileSize;
        if (moving) {
            const uint32_t duration = movementDurationMs();
            const int startX = static_cast<int>(startTileX) * kTileSize;
            const int startY = static_cast<int>(startTileY) * kTileSize;
            const int endX = static_cast<int>(targetTileX) * kTileSize;
            const int endY = static_cast<int>(targetTileY) * kTileSize;
            pixelX = startX + (endX - startX) * static_cast<int>(moveElapsedMs) / static_cast<int>(duration);
            pixelY = startY + (endY - startY) * static_cast<int>(moveElapsedMs) / static_cast<int>(duration);
            if (jumping) {
                const uint64_t elapsed = moveElapsedMs;
                const uint64_t lift = 4u * kJumpHeight * elapsed * (duration - elapsed) /
                    (static_cast<uint64_t>(duration) * duration);
                pixelY -= static_cast<int>(lift);
            }
        }
        player.position = {
            math::toScalar(static_cast<float>(originX + pixelX + 2)),
            math::toScalar(static_cast<float>(originY + pixelY)),
        };
    }

    void handlePortal() {
        const assets::Portal* activePortal = nullptr;
        for (std::size_t index = 0; index < assets::kPortalCount; ++index) {
            const assets::Portal& portal = assets::kPortals[index];
            if (portal.room == roomIndex && portal.x == tileX && portal.y == tileY) {
                activePortal = &portal;
                break;
            }
        }
        if (activePortal == nullptr) {
            portalLocked = false;
            return;
        }
        if (portalLocked) {
            return;
        }
        roomIndex = activePortal->targetRoom;
        tileX = activePortal->targetX;
        tileY = activePortal->targetY;
        ++roomChangeCount;
        portalLocked = true;
        configureRoom();
        syncPlayerPosition();
        centerCamera();
    }

    void centerCamera() {
        const assets::Room& room = assets::kRooms[roomIndex];
        if (!roomUsesCamera(room)) {
            pageTileX = 0;
            pageTileY = 0;
            camera.setPosition({math::toScalar(0), math::toScalar(0)});
            return;
        }
        const int maxPageX = static_cast<int>(room.width) > kViewportTiles
            ? static_cast<int>(room.width) - kViewportTiles
            : 0;
        const int maxPageY = static_cast<int>(room.height) > kViewportTiles
            ? static_cast<int>(room.height) - kViewportTiles
            : 0;
        int nextPageX = static_cast<int>(tileX) - kViewportTiles / 2;
        int nextPageY = static_cast<int>(tileY) - kViewportTiles / 2;
        if (nextPageX < 0) nextPageX = 0;
        if (nextPageY < 0) nextPageY = 0;
        if (nextPageX > maxPageX) nextPageX = maxPageX;
        if (nextPageY > maxPageY) nextPageY = maxPageY;
        pageTileX = static_cast<uint8_t>(nextPageX);
        pageTileY = static_cast<uint8_t>(nextPageY);
        camera.setPosition({
            math::toScalar(nextPageX * kTileSize),
            math::toScalar(nextPageY * kTileSize),
        });
    }

    void updateCamera() {
        if (!roomUsesCamera(assets::kRooms[roomIndex])) {
            return;
        }
        const math::Scalar targetCenterX =
            player.position.x + math::toScalar(player.width / 2);
        const math::Scalar targetCenterY =
            player.position.y + math::toScalar(player.height / 2);
        camera.followTarget({targetCenterX, targetCenterY});
        pageTileX = static_cast<uint8_t>(static_cast<int>(camera.getX()) / kTileSize);
        pageTileY = static_cast<uint8_t>(static_cast<int>(camera.getY()) / kTileSize);
    }

    PlayerActor player;
    gfx::TileMap4bpp tilemaps[assets::kMaxLayers]{};
    gfx::Camera2D camera{kViewportSize, kViewportSize};
    uint8_t roomIndex = assets::kWorldRoomIndex;
    uint8_t tileX = kWorldSpawnX;
    uint8_t tileY = kWorldSpawnY;
    uint8_t startTileX = kWorldSpawnX;
    uint8_t startTileY = kWorldSpawnY;
    uint8_t targetTileX = kWorldSpawnX;
    uint8_t targetTileY = kWorldSpawnY;
    uint8_t pageTileX = 0;
    uint8_t pageTileY = 0;
    uint32_t moveElapsedMs = 0;
    uint32_t roomChangeCount = 0;
    int originX = 0;
    int originY = 0;
    uint8_t heldButtons = 0;
    uint8_t activeButton = 0;
    bool moving = false;
    bool jumping = false;
    bool portalLocked = false;
};

} // namespace

struct h2_tuxemon {
    explicit h2_tuxemon(const h2_tuxemon_config_t& config) : scene(config.player) {}
    TuxemonScene scene;
};

int h2_tuxemon_create(const h2_tuxemon_config_t *config, h2_tuxemon_t **out_game) {
    if (out_game == nullptr) {
        return H2_TUXEMON_ERR_INVALID_ARG;
    }
    *out_game = nullptr;
    if (config == nullptr || config->player == nullptr) {
        return H2_TUXEMON_ERR_INVALID_ARG;
    }
    if (config->player->canvas.width == 0 || config->player->canvas.height == 0 ||
        config->player->canvas.width > kMaxPlayerCanvasSize ||
        config->player->canvas.height > kMaxPlayerCanvasSize) {
        return H2_TUXEMON_ERR_ASSET;
    }
    const size_t scratchSize = pixa_canvas_bgra_bytes(config->player->canvas);
    std::unique_ptr<uint8_t[]> scratch(new (std::nothrow) uint8_t[scratchSize]);
    if (scratch == nullptr) {
        return H2_TUXEMON_ERR_NO_MEMORY;
    }
    for (const char* clipName : {kRunLeftClip, kRunRightClip}) {
        pixa_clip_t clip{};
        if (pixa_find_clip(config->player, clipName, &clip) != PIXA_OK ||
            clip.frame_count == 0) {
            return H2_TUXEMON_ERR_ASSET;
        }
        for (uint32_t frame = 0; frame < clip.frame_count; ++frame) {
            if (pixa_decode_clip_frame_bgra(
                    config->player,
                    clipName,
                    frame,
                    scratch.get(),
                    scratchSize) != PIXA_OK) {
                return H2_TUXEMON_ERR_ASSET;
            }
        }
    }
    scratch.reset();
    *out_game = new (std::nothrow) h2_tuxemon(*config);
    if (*out_game == nullptr) {
        return H2_TUXEMON_ERR_NO_MEMORY;
    }
    if (!(*out_game)->scene.ready()) {
        delete *out_game;
        *out_game = nullptr;
        return H2_TUXEMON_ERR_NO_MEMORY;
    }
    return H2_TUXEMON_OK;
}

h2_game_scene_t *h2_tuxemon_scene(h2_tuxemon_t *game) {
    return game == nullptr ? nullptr : &game->scene;
}

void h2_tuxemon_handle_input(h2_tuxemon_t *game, const h2_game_input_event_t *event) {
    if (game != nullptr && event != nullptr) {
        game->scene.handleInput(*event);
    }
}

int h2_tuxemon_get_state(const h2_tuxemon_t *game, h2_tuxemon_state_t *out_state) {
    if (game == nullptr || out_state == nullptr) {
        return H2_TUXEMON_ERR_INVALID_ARG;
    }
    *out_state = game->scene.state();
    return H2_TUXEMON_OK;
}

void h2_tuxemon_destroy(h2_tuxemon_t *game) {
    delete game;
}
