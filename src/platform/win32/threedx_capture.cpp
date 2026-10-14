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

}  // namespace

const char* ThreeDxCapture::start(HWND window) {
    stop();
    if (!g_si.load()) return nullptr;  // not installed
    if (!g_si.initialized) {
        if (g_si.initialize() != SPW_NO_ERROR) return nullptr;  // not running
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
    return nullptr;
}

void ThreeDxCapture::stop() {
    if (!handle_) return;
    if (g_si.releaseDevice) g_si.releaseDevice(handle_);
    g_si.close(handle_);
    handle_ = nullptr;
}

bool ThreeDxCapture::handleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    if (!handle_) return false;
    SiGetEventData data;
    g_si.getEventWinInit(&data, message, wParam, lParam);
    alignas(16) static BYTE event[8192];  // SiSpwEvent, union sized for keyboard data
    return g_si.getEvent(handle_, 0, &data, event) == SI_IS_EVENT;
}
