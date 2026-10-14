#include "core/spacemouse.h"
#include <cstring>

static int16_t readInt16(const uint8_t* p) { return (int16_t)(p[0] | (p[1] << 8)); }

bool isSpaceMouse(uint16_t vendorId, uint16_t productId) {
    if (vendorId == 0x046D) return productId >= 0xC600 && productId <= 0xC6FF;  // 3Dconnexion under Logitech
    if (vendorId == 0x256F) return productId < 0xC650 || productId > 0xC657;    // excludes the CadMouse family
    return false;
}

bool parseSpaceMouseReport(const uint8_t* report, int length, SpaceMouseState& state) {
    if (length < 2) return false;
    SpaceMouseState before = state;
    switch (report[0]) {
    case 1:  // translation; newer devices append rotation to the same report
        if (length >= 7)
            for (int i = 0; i < 3; i++) state.axis[i] = readInt16(report + 1 + i * 2);
        if (length >= 13)
            for (int i = 0; i < 3; i++) state.axis[3 + i] = readInt16(report + 7 + i * 2);
        break;
    case 2:  // rotation
        if (length >= 7)
            for (int i = 0; i < 3; i++) state.axis[3 + i] = readInt16(report + 1 + i * 2);
        break;
    case 3: {  // button bitmask, little-endian
        uint32_t buttons = 0;
        for (int i = 1; i < length && i <= 4; i++) buttons |= (uint32_t)report[i] << (8 * (i - 1));
        state.buttons = buttons;
        break;
    }
    default: return false;
    }
    return memcmp(before.axis, state.axis, sizeof state.axis) != 0 || before.buttons != state.buttons;
}

const char* spaceMouseButtonName(uint16_t productId, int bit) {
    switch (productId) {
    case 0xC629:  // SpacePilot Pro
    case 0xC62B:  // SpaceMouse Pro
    case 0xC631:  // SpaceMouse Pro Wireless, cable
    case 0xC632:  // SpaceMouse Pro Wireless, receiver
        switch (bit) {
        case 0: return "Menu";
        case 1: return "Fit";
        case 2: return "Top";
        case 4: return "Right";
        case 5: return "Front";
        case 8: return "Roll";
        case 12: return "1";
        case 13: return "2";
        case 14: return "3";
        case 15: return "4";
        case 22: return "Esc";
        case 23: return "Alt";
        case 24: return "Shift";
        case 25: return "Ctrl";
        case 26: return "Lock";
        }
        return nullptr;
    case 0xC626:  // SpaceNavigator
    case 0xC628:  // SpaceNavigator for notebooks
    case 0xC62E:  // SpaceMouse Wireless, cable
    case 0xC62F:  // SpaceMouse Wireless, receiver
    case 0xC635:  // SpaceMouse Compact
        if (bit == 0) return "Left";
        if (bit == 1) return "Right";
        return nullptr;
    }
    return nullptr;
}

const char* spaceMouseModelName(uint16_t, uint16_t productId) {
    switch (productId) {
    case 0xC603: return "SpaceMouse Plus XT";
    case 0xC606: return "SpaceMouse Classic";
    case 0xC621: return "SpaceBall 5000";
    case 0xC623: return "SpaceTraveler";
    case 0xC625: return "SpacePilot";
    case 0xC626: return "SpaceNavigator";
    case 0xC627: return "SpaceExplorer";
    case 0xC628: return "SpaceNavigator NB";
    case 0xC629: return "SpacePilot Pro";
    case 0xC62B: return "SpaceMouse Pro";
    case 0xC62E:
    case 0xC62F: return "SpaceMouse Wireless";
    case 0xC631:
    case 0xC632: return "SpaceMouse Pro Wireless";
    case 0xC633: return "SpaceMouse Enterprise";
    case 0xC635: return "SpaceMouse Compact";
    case 0xC636: return "SpaceMouse Module";
    case 0xC652: return "3Dx Universal Receiver";
    }
    return "3D mouse";
}
