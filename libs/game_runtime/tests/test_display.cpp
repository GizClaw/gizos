#include "h2_game_runtime_internal.hpp"

#include <cassert>

namespace {

struct DisplayProbe {
    int drawCount = 0;
    int presentCount = 0;
    h2_display_rect_t lastRect{};
    size_t lastStride = 0;
};

int openDisplay(void *) {
    return H2_DISPLAY_OK;
}

int getDisplayInfo(void *, h2_display_info_t *info) {
    info->width = 240;
    info->height = 240;
    info->native_format = H2_DISPLAY_PIXEL_RGB565;
    return H2_DISPLAY_OK;
}

int drawBitmap(
    void *user,
    const h2_display_rect_t *rect,
    const void *,
    size_t stride,
    h2_display_pixel_format_t format) {
    auto *probe = static_cast<DisplayProbe *>(user);
    assert(format == H2_DISPLAY_PIXEL_RGB565);
    ++probe->drawCount;
    probe->lastRect = *rect;
    probe->lastStride = stride;
    return H2_DISPLAY_OK;
}

int presentDisplay(void *user) {
    ++static_cast<DisplayProbe *>(user)->presentCount;
    return H2_DISPLAY_OK;
}

int setBrightness(void *, uint32_t) {
    return H2_DISPLAY_OK;
}

int closeDisplay(void *) {
    return H2_DISPLAY_OK;
}

const h2_pal_display_vtable_t kDisplayVtable = {
    openDisplay,
    getDisplayInfo,
    drawBitmap,
    presentDisplay,
    setBrightness,
    closeDisplay,
};

} // namespace

int main() {
    DisplayProbe probe;
    const h2_pal_display_api_t display = {&probe, &kDisplayVtable};
    H2PixelRootDisplaySurface surface(&display, 240, 240);

    surface.clearBuffer();
    surface.drawFilledRectangle(0, 0, 8, 8, 0xf800u);
    surface.sendBuffer();
    assert(surface.last_status() == H2_GAME_RUNTIME_OK);
    assert(probe.drawCount == 1 && probe.presentCount == 1);
    assert(probe.lastRect.x == 0 && probe.lastRect.y == 0);
    assert(probe.lastRect.width == 240 && probe.lastRect.height == 240);
    assert(probe.lastStride == 240 * sizeof(uint16_t));

    surface.sendBuffer();
    assert(probe.drawCount == 1 && probe.presentCount == 1);

    surface.clearBuffer();
    surface.drawFilledRectangle(16, 0, 8, 8, 0xf800u);
    surface.sendBuffer();
    assert(probe.drawCount == 2 && probe.presentCount == 2);
    assert(probe.lastRect.x == 0 && probe.lastRect.y == 0);
    assert(probe.lastRect.width == 24 && probe.lastRect.height == 8);
    assert(probe.lastStride == 240 * sizeof(uint16_t));
    return 0;
}
