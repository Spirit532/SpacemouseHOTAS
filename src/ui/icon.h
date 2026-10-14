#pragma once
#include <cstdint>

// Renders the app icon (a joystick) as straight-alpha 0xAARRGGBB pixels. Green when the output is on.
void makeIcon(uint32_t* argb, int size, bool outputEnabled);
