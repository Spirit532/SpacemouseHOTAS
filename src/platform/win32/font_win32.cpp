// Rasterizes Segoe UI into the canvas glyph cache through GDI.
#include <windows.h>
#include <vector>
#include "ui/canvas.h"

bool loadSystemFont(FontAtlas& font, int pixelSize, bool bold) {
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    HFONT gdiFont =
        CreateFontW(-pixelSize, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                    OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ previous = SelectObject(dc, gdiFont);
    const MAT2 identity = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};
    std::vector<BYTE> bitmap;
    font.alpha.clear();
    for (int i = 0; i < 95; i++) {
        UINT ch = (UINT)(0x20 + i);
        GLYPHMETRICS metrics = {};
        FontGlyph& glyph = font.glyphs[i];
        DWORD size = GetGlyphOutlineW(dc, ch, GGO_GRAY8_BITMAP, &metrics, 0, nullptr, &identity);
        glyph.advance = metrics.gmCellIncX;
        if (size == GDI_ERROR || size == 0) continue;  // 0 = blank glyph such as space
        bitmap.resize(size);
        GetGlyphOutlineW(dc, ch, GGO_GRAY8_BITMAP, &metrics, size, bitmap.data(), &identity);
        glyph.width = (int)metrics.gmBlackBoxX;
        glyph.height = (int)metrics.gmBlackBoxY;
        glyph.left = metrics.gmptGlyphOrigin.x;
        glyph.top = -metrics.gmptGlyphOrigin.y;
        glyph.offset = (int)font.alpha.size();
        int pitch = (glyph.width + 3) & ~3;  // rows are DWORD aligned, coverage 0..64
        for (int row = 0; row < glyph.height; row++)
            for (int col = 0; col < glyph.width; col++) {
                int coverage = bitmap[row * pitch + col] * 255 / 64;
                font.alpha.push_back((uint8_t)(coverage > 255 ? 255 : coverage));
            }
    }
    font.capHeight = font.glyphs['H' - 0x20].height;
    SelectObject(dc, previous);
    DeleteObject(gdiFont);
    DeleteDC(dc);
    return font.capHeight > 0;
}
