#include "h2_game_text.h"

#include "graphics/Font5x7.h"

#include <climits>
#include <cstddef>
#include <cstdint>

namespace {

bool is_continuation(uint8_t value) {
    return (value & 0xC0u) == 0x80u;
}

int validate_surface(const h2_game_text_surface_t *surface) {
    if (surface == nullptr || surface->pixels == nullptr || surface->width_px == 0u ||
        surface->height_px == 0u || surface->stride_pixels < surface->width_px) {
        return H2_GAME_TEXT_ERR_INVALID_ARGUMENT;
    }
    if (surface->height_px > SIZE_MAX / surface->stride_pixels) {
        return H2_GAME_TEXT_ERR_OVERFLOW;
    }
    if (surface->height_px * surface->stride_pixels > surface->pixel_capacity) {
        return H2_GAME_TEXT_ERR_INVALID_ARGUMENT;
    }
    return H2_GAME_TEXT_OK;
}

int builtin_scale(uint16_t line_height_px, uint16_t *out_scale) {
    if (out_scale == nullptr || line_height_px == 0u || (line_height_px % 8u) != 0u) {
        return H2_GAME_TEXT_ERR_INVALID_ARGUMENT;
    }
    *out_scale = static_cast<uint16_t>(line_height_px / 8u);
    return H2_GAME_TEXT_OK;
}

int builtin_measure(
    void *,
    h2_game_text_span_t text,
    uint16_t line_height_px,
    h2_game_text_metrics_t *out_metrics) {
    uint16_t scale = 0;
    if (out_metrics == nullptr || builtin_scale(line_height_px, &scale) != H2_GAME_TEXT_OK) {
        return H2_GAME_TEXT_ERR_INVALID_ARGUMENT;
    }
    constexpr size_t kAdvance = 6u;
    if (text.byte_len > static_cast<size_t>(INT32_MAX) / (kAdvance * scale)) {
        return H2_GAME_TEXT_ERR_OVERFLOW;
    }
    for (size_t index = 0; index < text.byte_len; ++index) {
        const uint8_t value = static_cast<uint8_t>(text.data[index]);
        if (value < 32u || value > 126u) return H2_GAME_TEXT_ERR_UNSUPPORTED_GLYPH;
    }
    const size_t advance = text.byte_len * kAdvance * scale;
    out_metrics->width_px = text.byte_len == 0u
        ? 0
        : static_cast<int32_t>(advance - scale);
    out_metrics->advance_px = static_cast<int32_t>(advance);
    out_metrics->height_px = static_cast<int32_t>(7u * scale);
    out_metrics->baseline_px = static_cast<int32_t>(7u * scale);
    return H2_GAME_TEXT_OK;
}

int builtin_draw(
    void *,
    const h2_game_text_surface_t *surface,
    h2_game_text_span_t text,
    int32_t x,
    int32_t y,
    h2_game_text_style_t style) {
    uint16_t scale = 0;
    if (builtin_scale(style.line_height_px, &scale) != H2_GAME_TEXT_OK) {
        return H2_GAME_TEXT_ERR_INVALID_ARGUMENT;
    }
    h2_game_text_metrics_t metrics{};
    const int measurement = builtin_measure(nullptr, text, style.line_height_px, &metrics);
    if (measurement != H2_GAME_TEXT_OK) return measurement;

    for (size_t char_index = 0; char_index < text.byte_len; ++char_index) {
        const uint8_t value = static_cast<uint8_t>(text.data[char_index]);
        const pixelroot32::graphics::Sprite &glyph =
            pixelroot32::graphics::FONT5X7_GLYPHS[value - 32u];
        const int64_t glyph_x = static_cast<int64_t>(x) +
            static_cast<int64_t>(char_index) * 6 * scale;
        for (uint16_t row = 0; row < glyph.height; ++row) {
            const uint16_t bits = glyph.data[row];
            for (uint16_t column = 0; column < glyph.width; ++column) {
                const uint16_t bit_index = static_cast<uint16_t>(glyph.width - 1u - column);
                if ((bits & (static_cast<uint16_t>(1u) << bit_index)) == 0u) continue;
                for (uint16_t dy = 0; dy < scale; ++dy) {
                    const int64_t target_y = static_cast<int64_t>(y) + row * scale + dy;
                    if (target_y < 0 || target_y >= static_cast<int64_t>(surface->height_px)) continue;
                    for (uint16_t dx = 0; dx < scale; ++dx) {
                        const int64_t target_x = glyph_x + column * scale + dx;
                        if (target_x < 0 || target_x >= static_cast<int64_t>(surface->width_px)) continue;
                        surface->pixels[static_cast<size_t>(target_y) * surface->stride_pixels +
                            static_cast<size_t>(target_x)] = style.color_rgb565;
                    }
                }
            }
        }
    }
    return H2_GAME_TEXT_OK;
}

const h2_game_text_vtable_t kBuiltinVtable = {
    builtin_measure,
    builtin_draw,
};

} // namespace

extern "C" int h2_game_text_validate_utf8(h2_game_text_span_t text) {
    if (text.data == nullptr && text.byte_len != 0u) return H2_GAME_TEXT_ERR_INVALID_ARGUMENT;
    const auto *bytes = reinterpret_cast<const uint8_t *>(text.data);
    size_t index = 0;
    while (index < text.byte_len) {
        const uint8_t lead = bytes[index++];
        if (lead <= 0x7Fu) continue;

        uint32_t value = 0;
        size_t continuation_count = 0;
        uint32_t minimum = 0;
        if (lead >= 0xC2u && lead <= 0xDFu) {
            value = lead & 0x1Fu;
            continuation_count = 1u;
            minimum = 0x80u;
        } else if (lead >= 0xE0u && lead <= 0xEFu) {
            value = lead & 0x0Fu;
            continuation_count = 2u;
            minimum = 0x800u;
        } else if (lead >= 0xF0u && lead <= 0xF4u) {
            value = lead & 0x07u;
            continuation_count = 3u;
            minimum = 0x10000u;
        } else {
            return H2_GAME_TEXT_ERR_INVALID_UTF8;
        }
        if (continuation_count > text.byte_len - index) return H2_GAME_TEXT_ERR_INVALID_UTF8;
        for (size_t count = 0; count < continuation_count; ++count) {
            const uint8_t next = bytes[index++];
            if (!is_continuation(next)) return H2_GAME_TEXT_ERR_INVALID_UTF8;
            value = (value << 6u) | (next & 0x3Fu);
        }
        if (value < minimum || value > 0x10FFFFu || (value >= 0xD800u && value <= 0xDFFFu)) {
            return H2_GAME_TEXT_ERR_INVALID_UTF8;
        }
    }
    return H2_GAME_TEXT_OK;
}

extern "C" int h2_game_text_measure(
    const h2_game_text_api_t *api,
    h2_game_text_span_t text,
    uint16_t line_height_px,
    h2_game_text_metrics_t *out_metrics) {
    if (out_metrics == nullptr) return H2_GAME_TEXT_ERR_INVALID_ARGUMENT;
    *out_metrics = {};
    if (api == nullptr || api->vtable == nullptr || api->vtable->measure == nullptr ||
        line_height_px == 0u) {
        return H2_GAME_TEXT_ERR_INVALID_ARGUMENT;
    }
    const int validation = h2_game_text_validate_utf8(text);
    if (validation != H2_GAME_TEXT_OK) return validation;
    const int result = api->vtable->measure(
        api->user, text, line_height_px, out_metrics);
    if (result != H2_GAME_TEXT_OK) {
        *out_metrics = {};
        return result;
    }
    if (out_metrics->width_px < 0 || out_metrics->advance_px < 0 ||
        out_metrics->height_px < 0 ||
        out_metrics->baseline_px < 0 ||
        out_metrics->baseline_px > out_metrics->height_px) {
        *out_metrics = {};
        return H2_GAME_TEXT_ERR_PROVIDER;
    }
    return H2_GAME_TEXT_OK;
}

extern "C" int h2_game_text_draw(
    const h2_game_text_api_t *api,
    const h2_game_text_surface_t *surface,
    h2_game_text_span_t text,
    int32_t x,
    int32_t y,
    h2_game_text_style_t style) {
    if (api == nullptr || api->vtable == nullptr || api->vtable->draw == nullptr ||
        style.line_height_px == 0u) {
        return H2_GAME_TEXT_ERR_INVALID_ARGUMENT;
    }
    const int surface_result = validate_surface(surface);
    if (surface_result != H2_GAME_TEXT_OK) return surface_result;
    const int validation = h2_game_text_validate_utf8(text);
    if (validation != H2_GAME_TEXT_OK) return validation;
    return api->vtable->draw(api->user, surface, text, x, y, style);
}

extern "C" h2_game_text_api_t h2_game_text_builtin_5x7(void) {
    return {nullptr, &kBuiltinVtable};
}
