#include "ui/canvas.h"
#include <cstdlib>
#include <cstring>

static const int kFontSize = 12;  // logical pixels

bool Canvas::resize(int logicalWidth, int logicalHeight, int newScale) {
    if (newScale < 1) newScale = 1;
    if (pixels && width == logicalWidth * newScale && height == logicalHeight * newScale) return true;
    free(pixels);
    scale = newScale;
    width = logicalWidth * scale;
    height = logicalHeight * scale;
    pixels = (uint32_t*)malloc((size_t)width * height * 4);
    if (!pixels) {
        width = height = 0;
        return false;
    }
    if (fontScale_ != scale) {
        fonts_[0] = FontAtlas();
        fonts_[1] = FontAtlas();
        loadSystemFont(fonts_[0], kFontSize * scale, false);
        loadSystemFont(fonts_[1], kFontSize * scale, true);
        fontScale_ = scale;
    }
    return true;
}

void Canvas::release() {
    free(pixels);
    pixels = nullptr;
    width = height = 0;
    fonts_[0] = FontAtlas();
    fonts_[1] = FontAtlas();
    fontScale_ = 0;
}

void Canvas::fillDevice(int x0, int y0, int x1, int y1, uint32_t color) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > width) x1 = width;
    if (y1 > height) y1 = height;
    for (int y = y0; y < y1; y++) {
        uint32_t* row = pixels + (size_t)y * width;
        for (int x = x0; x < x1; x++) row[x] = color;
    }
}

void Canvas::fillRect(int x, int y, int w, int h, uint32_t color) {
    fillDevice(x * scale, y * scale, (x + w) * scale, (y + h) * scale, color);
}

void Canvas::strokeRect(int x, int y, int w, int h, uint32_t color) {
    fillRect(x, y, w, 1, color);
    fillRect(x, y + h - 1, w, 1, color);
    fillRect(x, y, 1, h, color);
    fillRect(x + w - 1, y, 1, h, color);
}

void Canvas::line(int x0, int y0, int x1, int y1, uint32_t color) {
    x0 *= scale;
    y0 *= scale;
    x1 *= scale;
    y1 *= scale;
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int thickness = scale > 1 ? scale / 2 + 1 : 1;
    for (;;) {
        fillDevice(x0, y0, x0 + thickness, y0 + thickness, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void Canvas::disc(int cx, int cy, int radius, uint32_t color) {
    int px = cx * scale + scale / 2, py = cy * scale + scale / 2, r = radius * scale + scale / 2;
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x * x + y * y <= r * r + r) fillDevice(px + x, py + y, px + x + 1, py + y + 1, color);
}

static const FontGlyph& glyphFor(const FontAtlas& font, char c) {
    unsigned code = (unsigned char)c;
    if (code < 0x20 || code > 0x7E) code = '?';
    return font.glyphs[code - 0x20];
}

int Canvas::runWidth(const char* s, int length, bool bold) const {
    int total = 0;
    for (int i = 0; i < length && s[i]; i++) total += glyphFor(fonts_[bold], s[i]).advance;
    return total;
}

int Canvas::textWidth(const char* s, bool bold) const {
    return (runWidth(s, (int)strlen(s), bold) + scale - 1) / scale;
}

void Canvas::drawRun(int x, int y, const char* s, int length, uint32_t color, bool bold) {
    const FontAtlas& font = fonts_[bold];
    if (!font.capHeight) return;
    int penX = x * scale;
    int baseline = y * scale + (8 * scale + font.capHeight) / 2;
    const uint32_t cr = (color >> 16) & 255, cg = (color >> 8) & 255, cb = color & 255;
    for (int i = 0; i < length && s[i]; i++) {
        const FontGlyph& g = glyphFor(font, s[i]);
        int gx = penX + g.left, gy = baseline + g.top;
        for (int row = 0; row < g.height; row++) {
            int py = gy + row;
            if (py < 0 || py >= height) continue;
            const uint8_t* coverage = font.alpha.data() + g.offset + row * g.width;
            uint32_t* dest = pixels + (size_t)py * width;
            for (int col = 0; col < g.width; col++) {
                int px = gx + col;
                uint32_t a = coverage[col];
                if (!a || px < 0 || px >= width) continue;
                uint32_t d = dest[px];
                uint32_t r = (((d >> 16) & 255) * (255 - a) + cr * a + 127) / 255;
                uint32_t gr = (((d >> 8) & 255) * (255 - a) + cg * a + 127) / 255;
                uint32_t b = ((d & 255) * (255 - a) + cb * a + 127) / 255;
                dest[px] = (r << 16) | (gr << 8) | b;
            }
        }
        penX += g.advance;
    }
}

int Canvas::text(int x, int y, const char* s, uint32_t color, bool bold) {
    int length = (int)strlen(s);
    drawRun(x, y, s, length, color, bold);
    return x + (runWidth(s, length, bold) + scale - 1) / scale;
}

int Canvas::textClipped(int x, int y, int maxWidth, const char* s, uint32_t color, bool bold) {
    int length = (int)strlen(s);
    int limit = maxWidth * scale;
    if (runWidth(s, length, bold) <= limit) return text(x, y, s, color, bold);
    int ellipsis = runWidth("...", 3, bold);
    while (length > 0 && runWidth(s, length, bold) + ellipsis > limit) length--;
    drawRun(x, y, s, length, color, bold);
    return text(x + runWidth(s, length, bold) / scale, y, "...", color, bold);
}
