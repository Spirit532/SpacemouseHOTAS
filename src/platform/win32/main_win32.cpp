// Windows host: tray icon, Raw Input, 3DxWare capture, the settings window and vJoy output.
// Event driven: timers run only while axes move, the window is open, or capture apps are watched.
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <hidsdi.h>
#include <string>
#include <vector>
#include "app.h"
#include "platform/win32/debug_tools.h"
#include "platform/win32/system_win32.h"
#include "platform/win32/threedx_capture.h"
#include "platform/win32/vjoy_pad.h"
#include "ui/icon.h"

#ifndef WM_GETDPISCALEDSIZE
#define WM_GETDPISCALEDSIZE 0x02E4
#endif

namespace {

const wchar_t* kHostClass = L"SpacemouseHOTAS_Host";
const wchar_t* kWindowClass = L"SpacemouseHOTAS_Window";
const UINT WM_TRAY_ICON = WM_APP + 1;
const UINT WM_SHOW_SETTINGS = WM_APP + 2;
const UINT WM_RUN_DEFERRED = WM_APP + 3;
enum TimerId { TIMER_UPDATE = 1, TIMER_PROCESS_POLL, TIMER_REDRAW };
enum MenuCommand { COMMAND_TOGGLE_OUTPUT = 1, COMMAND_TOGGLE_CAPTURE, COMMAND_OPEN_WINDOW, COMMAND_QUIT };
const DWORD kWindowStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
const COLORREF kBackgroundColor = RGB(0x1B, 0x1D, 0x21);

App g_app;
ThreeDxCapture g_capture;
VirtualPad* g_pad = nullptr;
HINSTANCE g_instance;
HWND g_host = nullptr, g_window = nullptr;
UINT g_taskbarCreatedMessage = 0;
HICON g_icons[2][2] = {};  // [output enabled][0 = small, 1 = large]
bool g_trayAdded = false;
bool g_trayShowsOutput = false, g_trayShowsCapture = false;
bool g_updateTimerRunning = false, g_pollTimerRunning = false;
UINT g_dpi = 96;
int g_scale = 1;
UiInput g_input;
bool g_trackingMouseLeave = false;
std::vector<HANDLE> g_devices;
double g_secondsPerTick = 0;

double now() {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart * g_secondsPerTick;
}

// DPI ----------------------------------------------------------------------------------------

UINT dpiForWindow(HWND window) {
    typedef UINT(WINAPI * GetDpiForWindowFn)(HWND);
    static auto getDpi = (GetDpiForWindowFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (getDpi) return getDpi(window);
    HDC dc = GetDC(nullptr);
    UINT dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(nullptr, dc);
    return dpi;
}

int scaleForDpi(UINT dpi) {
    if (g_app.profile.uiScale > 0) return g_app.profile.uiScale;
    int scale = (int)(dpi + 48) / 96;  // round to the nearest integer scale
    return scale < 1 ? 1 : scale;
}

SIZE windowSizeForDpi(UINT dpi) {
    typedef BOOL(WINAPI * AdjustForDpiFn)(RECT*, DWORD, BOOL, DWORD, UINT);
    static auto adjustForDpi =
        (AdjustForDpiFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi");
    int scale = scaleForDpi(dpi);
    RECT r = {0, 0, App::kWidth * scale, App::kHeight * scale};
    if (adjustForDpi)
        adjustForDpi(&r, kWindowStyle, FALSE, 0, dpi);
    else
        AdjustWindowRectEx(&r, kWindowStyle, FALSE, 0);
    return {r.right - r.left, r.bottom - r.top};
}

void enablePerMonitorDpiAwareness() {
    typedef BOOL(WINAPI * SetContextFn)(HANDLE);
    auto setContext = (SetContextFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (setContext && setContext((HANDLE)-4)) return;  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    SetProcessDPIAware();
}

// Tray ---------------------------------------------------------------------------------------

HICON createIcon(int size, bool outputEnabled) {
    std::vector<uint32_t> argb(size * size);
    makeIcon(argb.data(), size, outputEnabled);
    BITMAPV5HEADER header = {};
    header.bV5Size = sizeof header;
    header.bV5Width = size;
    header.bV5Height = -size;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&header, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color) return nullptr;
    memcpy(bits, argb.data(), argb.size() * 4);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO info = {TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

void updateTrayIcon(bool add) {
    bool output = g_app.profile.outputEnabled, capture = g_app.captureActive();
    NOTIFYICONDATAW data = {sizeof data};
    data.hWnd = g_host;
    data.uID = 1;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data.uCallbackMessage = WM_TRAY_ICON;
    data.hIcon = g_icons[output][0];
    swprintf(data.szTip, 128, L"SpacemouseHOTAS\nOutput %ls%ls", output ? L"on" : L"off",
             capture ? L", capturing" : L"");
    if (add)
        g_trayAdded = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
    else
        Shell_NotifyIconW(NIM_MODIFY, &data);
    g_trayShowsOutput = output;
    g_trayShowsCapture = capture;
    if (g_window) {
        SendMessageW(g_window, WM_SETICON, ICON_SMALL, (LPARAM)g_icons[output][0]);
        SendMessageW(g_window, WM_SETICON, ICON_BIG, (LPARAM)g_icons[output][1]);
    }
}

void removeTrayIcon() {
    if (!g_trayAdded) return;
    NOTIFYICONDATAW data = {sizeof data};
    data.hWnd = g_host;
    data.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &data);
    g_trayAdded = false;
}

void showTrayMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (g_app.profile.outputEnabled ? MF_CHECKED : 0), COMMAND_TOGGLE_OUTPUT,
                L"Output enabled");
    AppendMenuW(menu, MF_STRING | (g_app.profile.forceCapture ? MF_CHECKED : 0), COMMAND_TOGGLE_CAPTURE,
                L"Force capture");
    AppendMenuW(menu, MF_STRING, COMMAND_OPEN_WINDOW, L"Open window...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, COMMAND_QUIT, L"Quit");
    SetMenuDefaultItem(menu, COMMAND_OPEN_WINDOW, FALSE);
    POINT cursor;
    GetCursorPos(&cursor);
    SetForegroundWindow(g_host);  // required for the menu to close on outside clicks
    int command =
        TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, cursor.x, cursor.y, 0, g_host, nullptr);
    PostMessageW(g_host, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (command) PostMessageW(g_host, WM_COMMAND, command, 0);
}

// Window -------------------------------------------------------------------------------------

void resizeWindowForScale() {
    SIZE size = windowSizeForDpi(g_dpi);
    SetWindowPos(g_window, nullptr, 0, 0, size.cx, size.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(g_window, nullptr, FALSE);
}

void openWindow() {
    if (g_window) {
        ShowWindow(g_window, SW_RESTORE);
        SetForegroundWindow(g_window);
        return;
    }
    // Create on the monitor under the cursor so the window starts with that monitor's DPI.
    POINT cursor;
    GetCursorPos(&cursor);
    MONITORINFO monitor = {sizeof monitor};
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor);
    const RECT& area = monitor.rcWork;
    g_window = CreateWindowExW(0, kWindowClass, L"SpacemouseHOTAS", kWindowStyle, area.left, area.top, 100, 100,
                               nullptr, nullptr, g_instance, nullptr);
    if (!g_window) return;
    g_dpi = dpiForWindow(g_window);
    g_scale = scaleForDpi(g_dpi);
    SIZE size = windowSizeForDpi(g_dpi);
    int x = area.left + ((area.right - area.left) - size.cx) / 2;
    int y = area.top + ((area.bottom - area.top) - size.cy) / 2;
    SetWindowPos(g_window, nullptr, x, y > area.top ? y : area.top, size.cx, size.cy, SWP_NOZORDER | SWP_NOACTIVATE);
    bool output = g_app.profile.outputEnabled;
    SendMessageW(g_window, WM_SETICON, ICON_SMALL, (LPARAM)g_icons[output][0]);
    SendMessageW(g_window, WM_SETICON, ICON_BIG, (LPARAM)g_icons[output][1]);
    setDialogOwner(g_window);
    g_input = UiInput();
    g_app.redrawNeeded = true;
    SetTimer(g_window, TIMER_REDRAW, 33, nullptr);
    ShowWindow(g_window, SW_SHOWNORMAL);
    SetForegroundWindow(g_window);
}

void quit() {
    g_app.quitRequested = false;
    if (g_window) DestroyWindow(g_window);
    removeTrayIcon();
    g_app.shutdown();
    DestroyWindow(g_host);
}

// Keeps timers, tray and window in step with the app after every event.
void syncWithApp() {
    if (g_app.timedUpdatesNeeded != g_updateTimerRunning) {
        if (g_app.timedUpdatesNeeded)
            SetTimer(g_host, TIMER_UPDATE, 10, nullptr);
        else
            KillTimer(g_host, TIMER_UPDATE);
        g_updateTimerRunning = g_app.timedUpdatesNeeded;
    }
    bool pollNeeded = g_app.processPollingNeeded();
    if (pollNeeded != g_pollTimerRunning) {
        if (pollNeeded)
            SetTimer(g_host, TIMER_PROCESS_POLL, (UINT)(App::kProcessPollSeconds * 1000), nullptr);
        else
            KillTimer(g_host, TIMER_PROCESS_POLL);
        g_pollTimerRunning = pollNeeded;
    }
    if (g_trayAdded &&
        (g_trayShowsOutput != g_app.profile.outputEnabled || g_trayShowsCapture != g_app.captureActive()))
        updateTrayIcon(false);
    if (g_window && scaleForDpi(g_dpi) != g_scale) {
        g_scale = scaleForDpi(g_dpi);
        resizeWindowForScale();
    }
    if (g_app.hasDeferredActions()) PostMessageW(g_host, WM_RUN_DEFERRED, 0, 0);
    if (g_app.quitRequested) PostMessageW(g_host, WM_COMMAND, COMMAND_QUIT, 0);
}

// Input --------------------------------------------------------------------------------------

void onDeviceArrival(HANDLE device) {
    RID_DEVICE_INFO info = {sizeof info};
    UINT size = sizeof info;
    if (GetRawInputDeviceInfoW(device, RIDI_DEVICEINFO, &info, &size) == (UINT)-1 || info.dwType != RIM_TYPEHID) return;
    if (info.hid.usUsagePage != 1 || info.hid.usUsage != 8) return;  // multi-axis controller
    for (HANDLE known : g_devices)
        if (known == device) return;
    g_devices.push_back(device);

    std::string productName;
    wchar_t path[512];
    UINT pathLength = 512;
    if (GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, path, &pathLength) != (UINT)-1) {
        HANDLE file = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            wchar_t product[128] = {};
            if (HidD_GetProductString(file, product, sizeof product)) productName = toUtf8(product);
            CloseHandle(file);
        }
    }
    g_app.onDeviceArrived((uint16_t)info.hid.dwVendorId, (uint16_t)info.hid.dwProductId, productName.c_str());
}

void onDeviceRemoval(HANDLE device) {
    for (size_t i = 0; i < g_devices.size(); i++)
        if (g_devices[i] == device) {
            g_devices.erase(g_devices.begin() + i);
            g_app.onDeviceRemoved();
            g_app.update(now());
            return;
        }
}

void onRawInput(HRAWINPUT handle) {
    alignas(8) static BYTE buffer[2048];
    UINT size = sizeof buffer;
    if (GetRawInputData(handle, RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1) return;
    const RAWINPUT* input = (const RAWINPUT*)buffer;
    if (input->header.dwType != RIM_TYPEHID) return;
    const BYTE* report = input->data.hid.bRawData;
    for (DWORD i = 0; i < input->data.hid.dwCount; i++, report += input->data.hid.dwSizeHid)
        g_app.onDeviceReport(report, (int)input->data.hid.dwSizeHid);
    g_app.update(now());
}

// Capture apps are found when the foreground app changes, which costs nothing while idle.
void CALLBACK onForegroundChanged(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
    g_app.onForegroundAppChanged();
    syncWithApp();
}

const char* setDeviceCapture(bool capture) {
    if (!capture) {
        g_capture.stop();
        return nullptr;
    }
    return g_capture.start(g_host);
}

// Window procedures --------------------------------------------------------------------------

LRESULT CALLBACK hostProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (g_capture.handleMessage(message, wParam, lParam)) return 0;
    switch (message) {
    case WM_INPUT:
        onRawInput((HRAWINPUT)lParam);
        syncWithApp();
        break;  // DefWindowProc releases the raw input buffer
    case WM_INPUT_DEVICE_CHANGE:
        if (wParam == GIDC_ARRIVAL)
            onDeviceArrival((HANDLE)lParam);
        else
            onDeviceRemoval((HANDLE)lParam);
        syncWithApp();
        return 0;
    case WM_TIMER:
        if (wParam == TIMER_UPDATE)
            g_app.update(now());
        else if (wParam == TIMER_PROCESS_POLL)
            g_app.pollProcesses();
        syncWithApp();
        return 0;
    case WM_TRAY_ICON:
        if (LOWORD(lParam) == WM_LBUTTONUP)
            openWindow();
        else if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU)
            showTrayMenu();
        return 0;
    case WM_SHOW_SETTINGS: openWindow(); return 0;
    case WM_RUN_DEFERRED:
        g_app.runDeferredActions();
        syncWithApp();
        if (g_window) InvalidateRect(g_window, nullptr, FALSE);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case COMMAND_TOGGLE_OUTPUT: g_app.setOutputEnabled(!g_app.profile.outputEnabled); break;
        case COMMAND_TOGGLE_CAPTURE: g_app.setForceCapture(!g_app.profile.forceCapture); break;
        case COMMAND_OPEN_WINDOW: openWindow(); break;
        case COMMAND_QUIT: quit(); return 0;
        }
        syncWithApp();
        if (g_window) InvalidateRect(g_window, nullptr, FALSE);
        return 0;
    case WM_POWERBROADCAST:
        if (wParam == PBT_APMRESUMEAUTOMATIC) g_app.reconnectPad();
        return TRUE;
    case WM_ENDSESSION:
        if (wParam) g_app.shutdown();
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default:
        if (message == g_taskbarCreatedMessage && g_taskbarCreatedMessage) {  // Explorer restarted
            updateTrayIcon(true);
            return 0;
        }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void repaintNow(HWND window) {
    InvalidateRect(window, nullptr, FALSE);
    UpdateWindow(window);
}

void setMousePosition(LPARAM lParam) {
    g_input.mouseX = GET_X_LPARAM(lParam) / g_scale;
    g_input.mouseY = GET_Y_LPARAM(lParam) / g_scale;
}

void paintWindow(HWND window) {
    PAINTSTRUCT paint;
    HDC dc = BeginPaint(window, &paint);
    g_input.shiftDown = GetKeyState(VK_SHIFT) < 0;
    bool again = g_app.drawFrame(g_input, g_scale);
    g_input.clearEdges();
    const Canvas& canvas = g_app.canvas();
    if (canvas.pixels) {
        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof info.bmiHeader;
        info.bmiHeader.biWidth = canvas.width;
        info.bmiHeader.biHeight = -canvas.height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        SetDIBitsToDevice(dc, 0, 0, canvas.width, canvas.height, 0, 0, 0, canvas.height, canvas.pixels, &info,
                          DIB_RGB_COLORS);
    }
    // Fill any client area outside the canvas, e.g. mid DPI change.
    RECT client;
    GetClientRect(window, &client);
    HBRUSH brush = CreateSolidBrush(kBackgroundColor);
    RECT right = {canvas.width, 0, client.right, client.bottom},
         bottom = {0, canvas.height, canvas.width, client.bottom};
    if (right.left < right.right) FillRect(dc, &right, brush);
    if (bottom.top < bottom.bottom) FillRect(dc, &bottom, brush);
    DeleteObject(brush);
    EndPaint(window, &paint);
    if (again) InvalidateRect(window, nullptr, FALSE);
    syncWithApp();
}

int virtualKeyFromMessage(WPARAM wParam, LPARAM lParam) {
    int vk = (int)wParam;
    bool extended = (lParam >> 24) & 1;
    if (vk == VK_SHIFT) return (int)MapVirtualKeyW((UINT)((lParam >> 16) & 0xFF), MAPVK_VSC_TO_VK_EX);
    if (vk == VK_CONTROL) return extended ? VK_RCONTROL : VK_LCONTROL;
    if (vk == VK_MENU) return extended ? VK_RMENU : VK_LMENU;
    return vk;
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_PAINT: paintWindow(window); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_TIMER:
        if (wParam == TIMER_REDRAW && g_app.redrawNeeded && !IsIconic(window)) InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_MOUSEMOVE:
        setMousePosition(lParam);
        if (!g_trackingMouseLeave) {
            TRACKMOUSEEVENT track = {sizeof track, TME_LEAVE, window, 0};
            g_trackingMouseLeave = TrackMouseEvent(&track) != FALSE;
        }
        repaintNow(window);
        return 0;
    case WM_MOUSELEAVE:
        g_trackingMouseLeave = false;
        if (!g_input.leftDown) g_input.mouseX = g_input.mouseY = -1;
        repaintNow(window);
        return 0;
    case WM_LBUTTONDOWN:
        setMousePosition(lParam);
        SetCapture(window);
        g_input.leftDown = g_input.leftPressed = true;
        repaintNow(window);
        return 0;
    case WM_LBUTTONUP:
        setMousePosition(lParam);
        ReleaseCapture();
        g_input.leftDown = false;
        g_input.leftReleased = true;
        repaintNow(window);
        return 0;
    case WM_RBUTTONDOWN:
        setMousePosition(lParam);
        g_input.rightPressed = true;
        repaintNow(window);
        return 0;
    case WM_MBUTTONDOWN:
        setMousePosition(lParam);
        g_input.middlePressed = true;
        repaintNow(window);
        return 0;
    case WM_XBUTTONDOWN:
        setMousePosition(lParam);
        (GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? g_input.x1Pressed : g_input.x2Pressed) = true;
        repaintNow(window);
        return TRUE;
    case WM_MOUSEWHEEL: {
        POINT p = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(window, &p);
        g_input.mouseX = p.x / g_scale;
        g_input.mouseY = p.y / g_scale;
        g_input.wheel += GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
        repaintNow(window);
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (message == WM_SYSKEYDOWN && wParam == VK_F4) break;  // keep Alt+F4
        g_input.key = virtualKeyFromMessage(wParam, lParam);
        repaintNow(window);
        return 0;
    case WM_CHAR:
        if (wParam >= 0x20 && wParam < 0x7F) {
            g_input.text += (char)wParam;
            repaintNow(window);
        }
        return 0;
    case WM_GETDPISCALEDSIZE: {  // tell Windows our exact size at the new DPI so the move fits it
        SIZE* proposed = (SIZE*)lParam;
        *proposed = windowSizeForDpi(LOWORD(wParam));
        return TRUE;
    }
    case WM_DPICHANGED: {
        g_dpi = HIWORD(wParam);
        g_scale = scaleForDpi(g_dpi);
        const RECT* suggested = (const RECT*)lParam;
        SIZE size = windowSizeForDpi(g_dpi);
        SetWindowPos(window, nullptr, suggested->left, suggested->top, size.cx, size.cy, SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_CLOSE: DestroyWindow(window); return 0;
    case WM_DESTROY:
        KillTimer(window, TIMER_REDRAW);
        g_window = nullptr;
        setDialogOwner(nullptr);
        g_app.releaseCanvas();
        if (g_app.configDirty) g_app.save();
        SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);  // return UI pages to the OS
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

std::string configFilePath() {
    wchar_t appData[MAX_PATH];
    DWORD length = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
    std::wstring directory = (length && length < MAX_PATH) ? std::wstring(appData) : std::wstring(L".");
    directory += L"\\SpacemouseHOTAS";
    CreateDirectoryW(directory.c_str(), nullptr);
    return toUtf8((directory + L"\\config.ini").c_str());
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool showWindow = false;
    for (int i = 1; i < argc; i++) {
        const wchar_t* arg = argv[i];
        bool hasValue = i + 1 < argc;
        if (!wcscmp(arg, L"--configure-vjoy") && hasValue) return configureVJoyElevated(_wtoi(argv[i + 1]));
        if (!wcscmp(arg, L"--selftest") && hasValue) return runVJoySelfTest(argv[i + 1]);
        if (!wcscmp(arg, L"--write-icon") && hasValue) return writeIconFile(argv[i + 1]);
        if (!wcscmp(arg, L"--screenshot") && hasValue) {
            int scale = i + 2 < argc ? _wtoi(argv[i + 2]) : 1;
            int steps = argc - i - 3 > 0 ? argc - i - 3 : 0;
            return runScreenshot(argv[i + 1], scale, argv + i + 3, steps, configFilePath());
        }
        if (!wcscmp(arg, L"--show")) showWindow = true;
    }
    LocalFree(argv);

    HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\SpacemouseHOTAS_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(kHostClass, nullptr)) PostMessageW(other, WM_SHOW_SETTINGS, 0, 0);
        return 0;
    }

    if (!isVJoyInstalled()) {
        MessageBoxW(nullptr,
                    L"SpacemouseHOTAS requires the vJoy 2.2.x driver, which is not installed.\n\n"
                    L"Install it from https://github.com/BrunnerInnovation/vJoy/releases and start "
                    L"SpacemouseHOTAS again.",
                    L"SpacemouseHOTAS", MB_OK | MB_ICONERROR);
        return 1;
    }

    g_instance = instance;
    enablePerMonitorDpiAwareness();
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    g_secondsPerTick = 1.0 / (double)frequency.QuadPart;

    int smallIcon = GetSystemMetrics(SM_CXSMICON), largeIcon = GetSystemMetrics(SM_CXICON);
    for (int enabled = 0; enabled < 2; enabled++) {
        g_icons[enabled][0] = createIcon(smallIcon, enabled != 0);
        g_icons[enabled][1] = createIcon(largeIcon, enabled != 0);
    }

    WNDCLASSEXW windowClass = {sizeof windowClass};
    windowClass.lpfnWndProc = hostProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kHostClass;
    RegisterClassExW(&windowClass);
    windowClass.lpfnWndProc = windowProc;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = g_icons[1][1];
    windowClass.hIconSm = g_icons[1][0];
    RegisterClassExW(&windowClass);

    // Hidden top-level window (not message-only) so it also receives the TaskbarCreated broadcast.
    g_host = CreateWindowExW(WS_EX_TOOLWINDOW, kHostClass, L"SpacemouseHOTAS", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                             instance, nullptr);
    g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");

    PlatformHooks hooks;
    hooks.injectKey = injectKey;
    hooks.injectMouseButton = injectMouseButton;
    hooks.setDeviceCapture = setDeviceCapture;
    hooks.listProcesses = listProcesses;
    hooks.browseForExecutable = browseForExecutable;
    hooks.setStartWithSystem = setStartWithWindows;
    hooks.startWithSystemLabel = "Start with Windows";
    g_pad = createVJoyPad();
    g_app.init(g_pad, hooks, configFilePath());

    if (g_app.firstRun && g_pad->needsConfiguration()) {
        int answer = MessageBoxW(nullptr,
                                 L"vJoy is installed but not set up for SpacemouseHOTAS yet.\n\n"
                                 L"Configure it now? Windows will ask for administrator permission once.",
                                 L"SpacemouseHOTAS", MB_YESNO | MB_ICONQUESTION);
        if (answer == IDYES) {
            g_pad->configure();
            g_app.reconnectPad();
        }
    }

    RAWINPUTDEVICE device = {1, 8, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY,
                             g_host};  // generic desktop, multi-axis controller
    RegisterRawInputDevices(&device, 1, sizeof device);
    SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, onForegroundChanged, 0, 0,
                    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    updateTrayIcon(true);
    if (showWindow || g_app.firstRun || g_app.profile.openWindowAtStart || !g_trayAdded) openWindow();
    syncWithApp();
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    delete g_pad;
    ReleaseMutex(instanceMutex);
    CloseHandle(instanceMutex);
    return 0;
}
