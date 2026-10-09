#include "h2_mosaico_surface.h"
#include <assert.h>
#include <string.h>

static uint16_t surface[480 * 480];
int main(void) {
    for (size_t i = 0; i < 480 * 480; ++i) surface[i] = 0x1234;
    uint16_t pixel = 0xf800;
    int first = -1, end = -1;
    h2_display_rect_t rect = {479, 479, 1, 1};
    assert(h2_mosaico_surface_blit(surface, &rect, &pixel, 2, &first, &end) == 0);
    assert(first == 472 && end == 480 && surface[480 * 480 - 1] == 0x00f8);
    for (size_t i = 0; i < 480 * 480 - 1; ++i) assert(surface[i] == 0x1234);
    uint8_t padded[16] = {0};
    uint16_t values[] = {0x07e0, 0x001f, 0xffff, 0x0000};
    memcpy(padded + 1, values, 4);
    memcpy(padded + 7, values + 2, 4);
    rect = (h2_display_rect_t){3, 7, 2, 2};
    assert(h2_mosaico_surface_blit(surface, &rect, padded + 1, 6, &first, &end) == 0);
    assert(first == 0 && end == 16);
    assert(surface[7 * 480 + 3] == 0xe007 && surface[7 * 480 + 4] == 0x1f00);
    assert(surface[8 * 480 + 3] == 0xffff && surface[8 * 480 + 4] == 0);
    assert(surface[7 * 480 + 2] == 0x1234 && surface[8 * 480 + 5] == 0x1234);
    assert(h2_mosaico_surface_blit(surface, &rect, padded, SIZE_MAX, &first, &end) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_mosaico_surface_blit(surface, &rect, padded, 3, &first, &end) == H2_PAL_ERR_INVALID_ARG);
    rect.x = 479;
    assert(h2_mosaico_surface_blit(surface, &rect, padded, 6, &first, &end) == H2_PAL_ERR_INVALID_ARG);
    assert(surface[7 * 480 + 3] == 0xe007);
    return 0;
}
