// 3Dconnexion HID report parsing, shared by all platforms. Reports start with the report ID byte.
#pragma once
#include <cstdint>

struct SpaceMouseState {
    int16_t axis[6] = {};  // TX TY TZ RX RY RZ in the HID frame: X right, Y toward the user, Z down
    uint32_t buttons = 0;
    int connected = 0;  // number of connected devices
    uint16_t vid = 0, pid = 0;
    char name[64] = {};
};

// Vendor check only. Callers also require a multi-axis collection (usage page 1, usage 8), which skips
// CadMice, including the CadMouse half of the Universal Receiver.
bool isSpaceMouse(uint16_t vendorId, uint16_t productId);
bool hasMultiAxisCollection(const uint8_t* reportDescriptor, int length);
// Returns true if the state changed.
bool parseSpaceMouseReport(const uint8_t* report, int length, SpaceMouseState& state);
// Printed label of a button on known models, or nullptr.
const char* spaceMouseButtonName(uint16_t productId, int bit);
const char* spaceMouseModelName(uint16_t vendorId, uint16_t productId);
