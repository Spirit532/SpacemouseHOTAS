#include "platform/win32/threedx_capture.h"
#include <cstring>

namespace {

// Subset of the SiApp SDK (siapp.h, si.h, spwerror.h).
struct SiGetEventData {
    UINT msg;
    WPARAM wParam;
    LPARAM lParam;
};
enum { SI_ANY_DEVICE = -1, SI_EVENT = 1, SPW_NO_ERROR = 0, SI_IS_EVENT = 5 };
enum {
    SI_BUTTON_EVENT = 1,
    SI_MOTION_EVENT = 2,
    SI_ZERO_EVENT = 4,
    SI_BUTTON_PRESS_EVENT = 12,
    SI_BUTTON_RELEASE_EVENT = 13
};
// SiSpwEvent: int type, then a 4-byte aligned union. For button/motion events the union starts with
// SiButtonData {last, current, pressed, released} followed by long mData[6].
const int kEventButtonsCurrentOffset = 8;
const int kEventMotionOffset = 4 + 16;
const int kEventButtonNumberOffset = 4;

typedef int(__cdecl* SiInitializeFn)();
typedef void(__cdecl* SiOpenWinInitFn)(void* openData, HWND window);
typedef void*(__cdecl* SiOpenFn)(const char* appName, int deviceId, void* typeMask, int mode, void* openData);
typedef int(__cdecl* SiCloseFn)(void* handle);
typedef void(__cdecl* SiGetEventWinInitFn)(SiGetEventData* data, UINT msg, WPARAM wParam, LPARAM lParam);
typedef int(__cdecl* SiGetEventFn)(void* handle, int flags, SiGetEventData* data, void* event);
typedef int(__cdecl* SiGrabDeviceFn)(void* handle, int exclusive);
typedef int(__cdecl* SiReleaseDeviceFn)(void* handle);

struct SiApi {
    HMODULE library = nullptr;
    bool initialized = false;
    SiInitializeFn initialize = nullptr;
    SiOpenWinInitFn openWinInit = nullptr;
    SiOpenFn open = nullptr;
    SiCloseFn close = nullptr;
    SiGetEventWinInitFn getEventWinInit = nullptr;
    SiGetEventFn getEvent = nullptr;
    SiGrabDeviceFn grabDevice = nullptr;
    SiReleaseDeviceFn releaseDevice = nullptr;

    bool load() {
        if (library) return true;
        HMODULE dll = LoadLibraryW(L"siappdll.dll");
        if (!dll) return false;
#define LOAD_FN(field, name) field = (decltype(field))GetProcAddress(dll, name)
        LOAD_FN(initialize, "SiInitialize");
        LOAD_FN(openWinInit, "SiOpenWinInit");
        LOAD_FN(open, "SiOpen");
        LOAD_FN(close, "SiClose");
        LOAD_FN(getEventWinInit, "SiGetEventWinInit");
        LOAD_FN(getEvent, "SiGetEvent");
        LOAD_FN(grabDevice, "SiGrabDevice");
        LOAD_FN(releaseDevice, "SiReleaseDevice");
#undef LOAD_FN
        if (!initialize || !openWinInit || !open || !close || !getEventWinInit || !getEvent || !grabDevice) {
            FreeLibrary(dll);
            return false;
        }
        library = dll;
        return true;
    }
} g_si;

int16_t clampToInt16(long v) { return (int16_t)(v < -32767 ? -32767 : (v > 32767 ? 32767 : v)); }

}  // namespace

const char* ThreeDxCapture::start(HWND window) {
    stop();
    if (!g_si.load()) return "3DxWare not found";
    if (!g_si.initialized) {
        if (g_si.initialize() != SPW_NO_ERROR) return "3DxWare not running";
        g_si.initialized = true;
    }
    alignas(16) static BYTE openData[4096];  // SiOpenData, filled by SiOpenWinInit
    memset(openData, 0, sizeof openData);
    g_si.openWinInit(openData, window);
    handle_ = g_si.open("SpacemouseHOTAS", SI_ANY_DEVICE, nullptr, SI_EVENT, openData);
    if (!handle_) return "3DxWare connect failed";
    if (g_si.grabDevice(handle_, TRUE) != SPW_NO_ERROR) {
        stop();
        return "3DxWare refused the grab";
    }
    memset(axes, 0, sizeof axes);
    buttons = 0;
    return nullptr;
}

void ThreeDxCapture::stop() {
    if (!handle_) return;
    if (g_si.releaseDevice) g_si.releaseDevice(handle_);
    g_si.close(handle_);
    handle_ = nullptr;
}

bool ThreeDxCapture::handleMessage(UINT message, WPARAM wParam, LPARAM lParam, bool& stateChanged) {
    stateChanged = false;
    if (!handle_) return false;
    SiGetEventData data;
    g_si.getEventWinInit(&data, message, wParam, lParam);
    alignas(16) static BYTE event[8192];  // SiSpwEvent, union sized for keyboard data
    if (g_si.getEvent(handle_, 0, &data, event) != SI_IS_EVENT) return false;

    int type = *(int*)event;
    const int32_t* motion = (const int32_t*)(event + kEventMotionOffset);
    switch (type) {
    case SI_MOTION_EVENT:
        // 3DxWare frame (X right, Y up, Z toward user) to HID frame (X right, Y toward user, Z down).
        axes[0] = clampToInt16(motion[0]);
        axes[1] = clampToInt16(motion[2]);
        axes[2] = clampToInt16(-motion[1]);
        axes[3] = clampToInt16(motion[3]);
        axes[4] = clampToInt16(motion[5]);
        axes[5] = clampToInt16(-motion[4]);
        break;
    case SI_ZERO_EVENT: memset(axes, 0, sizeof axes); break;
    case SI_BUTTON_EVENT:  // bit n = button n, 1-based
        buttons = *(uint32_t*)(event + kEventButtonsCurrentOffset) >> 1;
        break;
    case SI_BUTTON_PRESS_EVENT:
    case SI_BUTTON_RELEASE_EVENT: {
        uint32_t number = *(uint32_t*)(event + kEventButtonNumberOffset);
        if (number >= 1 && number <= 32) {
            if (type == SI_BUTTON_PRESS_EVENT)
                buttons |= 1u << (number - 1);
            else
                buttons &= ~(1u << (number - 1));
        }
        break;
    }
    default: return true;
    }
    stateChanged = true;
    return true;
}
