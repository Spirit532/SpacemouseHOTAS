// Keeps 3DxWare from sending the 3D mouse to other apps, through its SiApp API (siappdll.dll, loaded at
// runtime). An exclusive SiGrabDevice routes the device to this app whatever is in the foreground.
// Input itself always comes from the device's HID reports; 3DxWare events are drained and dropped.
#pragma once
#include <windows.h>

class ThreeDxCapture {
public:
    // 3DxWare events arrive as window messages to `window`. Returns nullptr on success, or when 3DxWare
    // is not installed or not running (nothing else is taking the device then), or an error line.
    const char* start(HWND window);
    void stop();
    bool active() const { return handle_ != nullptr; }
    // Returns true if the message was a 3DxWare event, which the caller should then ignore.
    bool handleMessage(UINT message, WPARAM wParam, LPARAM lParam);

private:
    void* handle_ = nullptr;
};
