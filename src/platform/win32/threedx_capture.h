// 3D mouse capture through 3DxWare's SiApp API (siappdll.dll, loaded at runtime).
// An exclusive SiGrabDevice keeps 3DxWare routing the device to this app whatever is in the foreground.
#pragma once
#include <windows.h>
#include <cstdint>

class ThreeDxCapture {
public:
    // Events arrive as window messages to `window`. Returns nullptr on success or an error line.
    const char* start(HWND window);
    void stop();
    bool active() const { return handle_ != nullptr; }
    // Returns true if the message belonged to 3DxWare; axes/buttons are updated when it carried data.
    bool handleMessage(UINT message, WPARAM wParam, LPARAM lParam, bool& stateChanged);

    int16_t axes[6] = {};  // raw HID frame, matching SpaceMouse reports
    uint32_t buttons = 0;

private:
    void* handle_ = nullptr;
};
