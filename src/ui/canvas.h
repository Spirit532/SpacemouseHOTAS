// Software renderer into a 0x00RRGGBB pixel buffer.
// Drawing calls take logical coordinates; the buffer is logical size * scale, so text and lines
// stay sharp at any integer DPI scale.
#pragma once
#include <cstdint>
#include <vector>

struct FontGlyph {
    int width = 0, height = 0;  // bitmap size, device pixels
    int left = 0, top = 0;      // bitmap offset from the pen position on the baseline
    int advance = 0;            // device pixels
    int offset = 0;             // start in FontAtlas::alpha
};

struct FontAtlas {
    int capHeight = 0;     // device pixels; 0 = not loaded
    FontGlyph glyphs[95];  // ASCII 0x20..0x7E
    std::vector<uint8_t> alpha;
};

// Implemented per platform: rasterize the system UI font at the given pixel size.
bool loadSystemFont(FontAtlas& font, int pixelSize, bool bold);

struct Canvas {
    uint32_t* pixels = nullptr;
    int width = 0, height = 0;  // device pixels
    int scale = 1;

    bool resize(int logicalWidth, int logicalHeight, int scale);
    void release();
    int logicalWidth() const { return width / scale; }
    int logicalHeight() const { return height / scale; }

    void fillRect(int x, int y, int w, int h, uint32_t color);
    void strokeRect(int x, int y, int w, int h, uint32_t color);
    void line(int x0, int y0, int x1, int y1, uint32_t color);
    void disc(int cx, int cy, int radius, uint32_t color);

    // `y` is the top of an 8 px line box; capitals are centered in it. Returns the x after the text.
    int text(int x, int y, const char* s, uint32_t color, bool bold = false);
    // Same, cut with "..." to fit maxWidth.
    int textClipped(int x, int y, int maxWidth, const char* s, uint32_t color, bool bold = false);
    int textWidth(const char* s, bool bold = false) const;

private:
    FontAtlas fonts_[2];  // regular, bold
    int fontScale_ = 0;
    void fillDevice(int x0, int y0, int x1, int y1, uint32_t color);
    int runWidth(const char* s, int length, bool bold) const;
    void drawRun(int x, int y, const char* s, int length, uint32_t color, bool bold);
};

// Linear blend, t in 0..256.
inline uint32_t blendColor(uint32_t a, uint32_t b, int t) {
    uint32_t r = (((a >> 16) & 255) * (256 - t) + ((b >> 16) & 255) * t) >> 8;
    uint32_t g = (((a >> 8) & 255) * (256 - t) + ((b >> 8) & 255) * t) >> 8;
    uint32_t bl = ((a & 255) * (256 - t) + (b & 255) * t) >> 8;
    return (r << 16) | (g << 8) | bl;
}
