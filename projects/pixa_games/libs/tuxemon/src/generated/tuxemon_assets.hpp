#pragma once

#include "graphics/Renderer.h"

#include <cstddef>
#include <cstdint>

namespace h2::tuxemon::assets {

constexpr uint8_t kMaxLayers = 5;
constexpr uint8_t kDirectionUp = 0x01;
constexpr uint8_t kDirectionDown = 0x02;
constexpr uint8_t kDirectionLeft = 0x04;
constexpr uint8_t kDirectionRight = 0x08;

struct Layer {
    const uint8_t* indices;
    const pixelroot32::graphics::Sprite4bpp* tiles;
    uint16_t tileCount;
};

struct Room {
    const char* name;
    uint8_t width;
    uint8_t height;
    uint8_t layerCount;
    bool isWorld;
    Layer layers[kMaxLayers];
    // Low nibble: allowed entry sides. High nibble: allowed exit directions.
    const uint8_t* traversal;
};

struct Portal {
    uint8_t room;
    uint8_t x;
    uint8_t y;
    uint8_t targetRoom;
    uint8_t targetX;
    uint8_t targetY;
};

extern const uint16_t kBackgroundPalette[16];
extern const Room kRooms[];
extern const std::size_t kRoomCount;
extern const Portal kPortals[];
extern const std::size_t kPortalCount;
extern const uint8_t kWorldRoomIndex;

} // namespace h2::tuxemon::assets
