// Rasterizes the desktop's sans-serif font through Xft into the canvas glyph cache.
// Each glyph is drawn white on black into a pixmap and read back; green becomes coverage.
#include <X11/Xft/Xft.h>
#include <X11/Xutil.h>
#include "platform/linux/x11_context.h"
#include "ui/canvas.h"

bool loadSystemFont(FontAtlas& font, int pixelSize, bool bold) {
    Display* display = g_x11.display;
    if (!display) return false;
    XftFont* xftFont =
        XftFontOpen(display, g_x11.screen, XFT_FAMILY, XftTypeString, "sans-serif", XFT_PIXEL_SIZE, XftTypeDouble,
                    (double)pixelSize, XFT_WEIGHT, XftTypeInteger, bold ? XFT_WEIGHT_DEMIBOLD : XFT_WEIGHT_MEDIUM,
                    XFT_ANTIALIAS, XftTypeBool, True, nullptr);
    if (!xftFont) return false;
    Visual* visual = DefaultVisual(display, g_x11.screen);
    Colormap colormap = DefaultColormap(display, g_x11.screen);
    int depth = DefaultDepth(display, g_x11.screen);
    XRenderColor whiteValue = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
    XftColor white;
    XftColorAllocValue(display, visual, colormap, &whiteValue, &white);
    font.alpha.clear();
    for (int i = 0; i < 95; i++) {
        FcChar8 ch = (FcChar8)(0x20 + i);
        XGlyphInfo info;
        XftTextExtents8(display, xftFont, &ch, 1, &info);
        FontGlyph& glyph = font.glyphs[i];
        glyph.advance = info.xOff;
        if (info.width == 0 || info.height == 0) continue;
        Pixmap pixmap = XCreatePixmap(display, g_x11.root, info.width, info.height, depth);
        GC gc = XCreateGC(display, pixmap, 0, nullptr);
        XSetForeground(display, gc, 0);
        XFillRectangle(display, pixmap, gc, 0, 0, info.width, info.height);
        XftDraw* draw = XftDrawCreate(display, pixmap, visual, colormap);
        XftDrawString8(draw, &white, xftFont, info.x, info.y, &ch, 1);
        XftDrawDestroy(draw);
        XImage* image = XGetImage(display, pixmap, 0, 0, info.width, info.height, AllPlanes, ZPixmap);
        glyph.width = info.width;
        glyph.height = info.height;
        glyph.left = -info.x;
        glyph.top = -info.y;
        glyph.offset = (int)font.alpha.size();
        for (int y = 0; y < glyph.height; y++)
            for (int x = 0; x < glyph.width; x++)
                font.alpha.push_back(image ? (uint8_t)((XGetPixel(image, x, y) >> 8) & 255) : 0);
        if (image) XDestroyImage(image);
        XFreeGC(display, gc);
        XFreePixmap(display, pixmap);
    }
    font.capHeight = font.glyphs['H' - 0x20].height;
    XftColorFree(display, visual, colormap, &white);
    XftFontClose(display, xftFont);
    return font.capHeight > 0;
}
