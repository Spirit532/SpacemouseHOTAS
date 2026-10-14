// Keyboard keys and mouse buttons usable as mapping targets.
// The canonical key id is the Windows virtual-key code; each entry carries the other platforms' codes.
#pragma once
#include <cstdint>

struct KeyDef {
    uint8_t vk;
    const char* name;
    uint16_t linuxKey;   // KEY_* from linux/input-event-codes.h
    uint8_t macKey;      // kVK_* from Carbon HIToolbox
    uint32_t x11Keysym;  // unshifted keysym
};

extern const KeyDef kKeys[];
extern const int kKeyCount;

const KeyDef* keyByVk(int vk);
const KeyDef* keyByX11Keysym(uint32_t keysym);
const KeyDef* keyByMacKeyCode(int keyCode);

enum {
    VK_CODE_BACKSPACE = 0x08,
    VK_CODE_ENTER = 0x0D,
    VK_CODE_ESCAPE = 0x1B,
    VK_CODE_UP = 0x26,
    VK_CODE_DOWN = 0x28,
    VK_CODE_DELETE = 0x2E
};

enum MouseButton { MOUSE_LEFT, MOUSE_RIGHT, MOUSE_MIDDLE, MOUSE_X1, MOUSE_X2, MOUSE_BUTTON_COUNT };
const char* mouseButtonName(int button);
