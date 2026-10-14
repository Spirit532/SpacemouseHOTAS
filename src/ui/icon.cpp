#include "ui/icon.h"

enum Coverage { EMPTY, OUTLINE, FILL };

// Joystick shape in unit coordinates: ball on a stick over a base, with a dark outline around it.
static Coverage shapeAt(float x, float y) {
    auto inBall = [](float x, float y, float radius) {
        float dx = x - 0.5f, dy = y - 0.27f;
        return dx * dx + dy * dy <= radius * radius;
    };
    auto inStick = [](float x, float y, float grow) {
        return x >= 0.43f - grow && x <= 0.57f + grow && y >= 0.27f && y <= 0.74f + grow;
    };
    auto inBase = [](float x, float y, float grow) {
        return x >= 0.12f - grow && x <= 0.88f + grow && y >= 0.70f - grow && y <= 0.90f + grow;
    };
    if (inBall(x, y, 0.21f) || inStick(x, y, 0) || inBase(x, y, 0)) return FILL;
    const float outline = 0.07f;
    if (inBall(x, y, 0.21f + outline) || inStick(x, y, outline) || inBase(x, y, outline)) return OUTLINE;
    return EMPTY;
}

void makeIcon(uint32_t* argb, int size, bool outputEnabled) {
    const uint32_t fillColor = outputEnabled ? 0x4CC979 : 0x9AA0A8;
    const uint32_t outlineColor = 0x15171A;
    const int samples = 4;  // 4x4 supersampling per pixel
    for (int py = 0; py < size; py++) {
        for (int px = 0; px < size; px++) {
            int fillHits = 0, outlineHits = 0;
            for (int sy = 0; sy < samples; sy++)
                for (int sx = 0; sx < samples; sx++) {
                    Coverage c = shapeAt((px + (sx + 0.5f) / samples) / size, (py + (sy + 0.5f) / samples) / size);
                    if (c == FILL)
                        fillHits++;
                    else if (c == OUTLINE)
                        outlineHits++;
                }
            int hits = fillHits + outlineHits;
            uint32_t alpha = (uint32_t)(hits * 255 / (samples * samples));
            uint32_t rgb = 0;
            if (hits) {
                auto channel = [&](int shift) {
                    return (((fillColor >> shift) & 255) * fillHits + ((outlineColor >> shift) & 255) * outlineHits) /
                           hits;
                };
                rgb = (channel(16) << 16) | (channel(8) << 8) | channel(0);
            }
            argb[py * size + px] = (alpha << 24) | rgb;
        }
    }
}
