#include "h2_game_runtime_internal.hpp"

namespace {

constexpr int kDirtyCellSize = 8;

uint32_t hashCell(const uint16_t *pixels, int stride, int x, int y, int width, int height) {
    uint32_t hash = 2166136261u;
    for (int row = 0; row < height; ++row) {
        const uint16_t *source = pixels + static_cast<size_t>(y + row) * static_cast<size_t>(stride) + x;
        for (int column = 0; column < width; ++column) {
            hash ^= source[column];
            hash *= 16777619u;
        }
    }
    return hash;
}

} // namespace

H2PixelRootDisplaySurface::H2PixelRootDisplaySurface(const h2_pal_display_api_t *display, int width, int height)
    : display_(display), width_(width), height_(height) {
    const size_t pixel_count = static_cast<size_t>(width_) * static_cast<size_t>(height_);
    pixels_ = new (std::nothrow) uint16_t[pixel_count];
    if (pixels_ == nullptr) {
        last_status_ = H2_GAME_RUNTIME_ERR_NO_MEMORY;
        return;
    }
    hash_columns_ = (width_ + kDirtyCellSize - 1) / kDirtyCellSize;
    hash_rows_ = (height_ + kDirtyCellSize - 1) / kDirtyCellSize;
    const size_t hash_count = static_cast<size_t>(hash_columns_) * static_cast<size_t>(hash_rows_);
    cell_hashes_ = new (std::nothrow) uint32_t[hash_count];
    if (cell_hashes_ == nullptr) {
        delete[] pixels_;
        pixels_ = nullptr;
        last_status_ = H2_GAME_RUNTIME_ERR_NO_MEMORY;
    }
}

H2PixelRootDisplaySurface::~H2PixelRootDisplaySurface() {
    delete[] cell_hashes_;
    delete[] pixels_;
}

void H2PixelRootDisplaySurface::init() {}

void H2PixelRootDisplaySurface::setRotation(uint16_t rotation) {
    (void)rotation;
}

void H2PixelRootDisplaySurface::clearBuffer() {
    if (pixels_ == nullptr) {
        return;
    }
    const size_t pixel_count = static_cast<size_t>(width_) * static_cast<size_t>(height_);
    for (size_t i = 0; i < pixel_count; ++i) {
        pixels_[i] = 0;
    }
}

void H2PixelRootDisplaySurface::sendBuffer() {
    if (display_ == nullptr || pixels_ == nullptr || cell_hashes_ == nullptr) {
        last_status_ = H2_GAME_RUNTIME_ERR_INVALID_ARG;
        return;
    }

    int first_changed_column = hash_columns_;
    int first_changed_row = hash_rows_;
    int last_changed_column = -1;
    int last_changed_row = -1;
    for (int row = 0; row < hash_rows_; ++row) {
        for (int column = 0; column < hash_columns_; ++column) {
            const int x = column * kDirtyCellSize;
            const int y = row * kDirtyCellSize;
            const int cell_width = width_ - x < kDirtyCellSize ? width_ - x : kDirtyCellSize;
            const int cell_height = height_ - y < kDirtyCellSize ? height_ - y : kDirtyCellSize;
            const size_t hash_index = static_cast<size_t>(row) * static_cast<size_t>(hash_columns_) +
                static_cast<size_t>(column);
            const uint32_t hash = hashCell(pixels_, width_, x, y, cell_width, cell_height);
            if (!has_presented_frame_ || cell_hashes_[hash_index] != hash) {
                if (column < first_changed_column) first_changed_column = column;
                if (row < first_changed_row) first_changed_row = row;
                if (column > last_changed_column) last_changed_column = column;
                if (row > last_changed_row) last_changed_row = row;
            }
            cell_hashes_[hash_index] = hash;
        }
    }

    if (last_changed_column < 0 || last_changed_row < 0) {
        last_status_ = H2_GAME_RUNTIME_OK;
        return;
    }

    h2_display_rect_t rect{};
    rect.x = first_changed_column * kDirtyCellSize;
    rect.y = first_changed_row * kDirtyCellSize;
    rect.width = (last_changed_column - first_changed_column + 1) * kDirtyCellSize;
    rect.height = (last_changed_row - first_changed_row + 1) * kDirtyCellSize;
    if (rect.x + rect.width > width_) rect.width = width_ - rect.x;
    if (rect.y + rect.height > height_) rect.height = height_ - rect.y;
    const uint16_t *changed_pixels = pixels_ +
        static_cast<size_t>(rect.y) * static_cast<size_t>(width_) + static_cast<size_t>(rect.x);
    const int draw_rc = h2_pal_display_draw_bitmap(
        display_,
        &rect,
        changed_pixels,
        static_cast<size_t>(width_) * sizeof(uint16_t),
        H2_DISPLAY_PIXEL_RGB565);
    if (draw_rc != H2_DISPLAY_OK) {
        has_presented_frame_ = false;
        last_status_ = H2_GAME_RUNTIME_ERR_DISPLAY;
        return;
    }

    const int present_rc = h2_pal_display_present(display_);
    if (present_rc != H2_DISPLAY_OK) {
        has_presented_frame_ = false;
        last_status_ = H2_GAME_RUNTIME_ERR_DISPLAY;
        return;
    }
    has_presented_frame_ = true;
    last_status_ = H2_GAME_RUNTIME_OK;
}

void H2PixelRootDisplaySurface::drawFilledCircle(int x, int y, int radius, uint16_t color) {
    if (radius < 0) {
        return;
    }
    for (int yy = y - radius; yy <= y + radius; ++yy) {
        for (int xx = x - radius; xx <= x + radius; ++xx) {
            const int dx = xx - x;
            const int dy = yy - y;
            if (dx * dx + dy * dy <= radius * radius) {
                putPixel(xx, yy, color);
            }
        }
    }
}

void H2PixelRootDisplaySurface::drawCircle(int x, int y, int radius, uint16_t color) {
    if (radius < 0) {
        return;
    }
    int f = 1 - radius;
    int ddx = 1;
    int ddy = -2 * radius;
    int xx = 0;
    int yy = radius;
    putPixel(x, y + radius, color);
    putPixel(x, y - radius, color);
    putPixel(x + radius, y, color);
    putPixel(x - radius, y, color);
    while (xx < yy) {
        if (f >= 0) {
            yy--;
            ddy += 2;
            f += ddy;
        }
        xx++;
        ddx += 2;
        f += ddx;
        putPixel(x + xx, y + yy, color);
        putPixel(x - xx, y + yy, color);
        putPixel(x + xx, y - yy, color);
        putPixel(x - xx, y - yy, color);
        putPixel(x + yy, y + xx, color);
        putPixel(x - yy, y + xx, color);
        putPixel(x + yy, y - xx, color);
        putPixel(x - yy, y - xx, color);
    }
}

void H2PixelRootDisplaySurface::drawRectangle(int x, int y, int width, int height, uint16_t color) {
    drawLine(x, y, x + width - 1, y, color);
    drawLine(x, y + height - 1, x + width - 1, y + height - 1, color);
    drawLine(x, y, x, y + height - 1, color);
    drawLine(x + width - 1, y, x + width - 1, y + height - 1, color);
}

void H2PixelRootDisplaySurface::drawFilledRectangle(int x, int y, int width, int height, uint16_t color) {
    if (pixels_ == nullptr || width <= 0 || height <= 0) {
        return;
    }
    int x0 = x;
    int y0 = y;
    int x1 = x + width;
    int y1 = y + height;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > width_) x1 = width_;
    if (y1 > height_) y1 = height_;
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    for (int yy = y0; yy < y1; ++yy) {
        uint16_t *row = pixels_ + static_cast<size_t>(yy) * static_cast<size_t>(width_);
        for (int xx = x0; xx < x1; ++xx) {
            row[xx] = color;
        }
    }
}

void H2PixelRootDisplaySurface::drawLine(int x1, int y1, int x2, int y2, uint16_t color) {
    const int dx = x2 > x1 ? x2 - x1 : x1 - x2;
    const int sx = x1 < x2 ? 1 : -1;
    const int dy = y2 > y1 ? y1 - y2 : y2 - y1;
    const int sy = y1 < y2 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        putPixel(x1, y1, color);
        if (x1 == x2 && y1 == y2) {
            break;
        }
        const int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x1 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y1 += sy;
        }
    }
}

void H2PixelRootDisplaySurface::drawBitmap(int x, int y, int width, int height, const uint8_t *bitmap, uint16_t color) {
    if (bitmap == nullptr || width <= 0 || height <= 0) {
        return;
    }
    for (int yy = 0; yy < height; ++yy) {
        for (int xx = 0; xx < width; ++xx) {
            const int bit_index = yy * width + xx;
            if ((bitmap[bit_index / 8] & (0x80 >> (bit_index % 8))) != 0) {
                putPixel(x + xx, y + yy, color);
            }
        }
    }
}

void H2PixelRootDisplaySurface::drawPixel(int x, int y, uint16_t color) {
    putPixel(x, y, color);
}

uint16_t *H2PixelRootDisplaySurface::getPixelBuffer() {
    return pixels_;
}

void H2PixelRootDisplaySurface::setContrast(uint8_t level) {
    (void)level;
}

void H2PixelRootDisplaySurface::setTextColor(uint16_t color) {
    (void)color;
}

void H2PixelRootDisplaySurface::setTextSize(uint8_t size) {
    (void)size;
}

void H2PixelRootDisplaySurface::setCursor(int16_t x, int16_t y) {
    (void)x;
    (void)y;
}

uint16_t H2PixelRootDisplaySurface::color565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r & 0xF8u) << 8u) | ((g & 0xFCu) << 3u) | (b >> 3u));
}

void H2PixelRootDisplaySurface::setDisplaySize(int width, int height) {
    width_ = width;
    height_ = height;
}

void H2PixelRootDisplaySurface::present() {
    sendBuffer();
}

int H2PixelRootDisplaySurface::last_status() const {
    return last_status_;
}

void H2PixelRootDisplaySurface::putPixel(int x, int y, uint16_t color) {
    if (pixels_ == nullptr || x < 0 || y < 0 || x >= width_ || y >= height_) {
        return;
    }
    pixels_[static_cast<size_t>(y) * static_cast<size_t>(width_) + static_cast<size_t>(x)] = color;
}
