#include "platform/win32/system_win32.h"
#include <commdlg.h>
#include <tlhelp32.h>
#include "core/keys.h"

static HWND g_dialogOwner = nullptr;

void setDialogOwner(HWND window) { g_dialogOwner = window; }

std::string toUtf8(const wchar_t* text) {
    int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) return std::string();
    std::string result(length - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, &result[0], length, nullptr, nullptr);
    return result;
}

std::wstring toWide(const std::string& text) {
    int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (length <= 1) return std::wstring();
    std::wstring result(length - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &result[0], length);
    return result;
}

// Scancodes, so games reading DirectInput or Raw Input see the keys too.
void injectKey(int vk, bool down) {
    INPUT event = {};
    event.type = INPUT_KEYBOARD;
    event.ki.wScan = (WORD)MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    event.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
    switch (vk) {
    case VK_INSERT:
    case VK_DELETE:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
    case VK_RCONTROL:
    case VK_RMENU:
    case VK_DIVIDE: event.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY; break;
    }
    SendInput(1, &event, sizeof event);
}

void injectMouseButton(int button, bool down) {
    INPUT event = {};
    event.type = INPUT_MOUSE;
    switch (button) {
    case MOUSE_LEFT: event.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
    case MOUSE_RIGHT: event.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
    case MOUSE_MIDDLE: event.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
    case MOUSE_X1:
    case MOUSE_X2:
        event.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        event.mi.mouseData = button == MOUSE_X1 ? XBUTTON1 : XBUTTON2;
        break;
    default: return;
    }
    SendInput(1, &event, sizeof event);
}

void listProcesses(std::vector<std::string>& executableNames) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W entry = {sizeof entry};
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
        executableNames.push_back(toUtf8(entry.szExeFile));
    CloseHandle(snapshot);
}

bool browseForExecutable(std::string& path) {
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW dialog = {sizeof dialog};
    dialog.hwndOwner = g_dialogOwner;
    dialog.lpstrFilter = L"Programs (*.exe)\0*.exe\0All files\0*.*\0";
    dialog.lpstrFile = file;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrTitle = L"Choose an app that triggers capture";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) return false;
    path = toUtf8(file);
    return true;
}

void setStartWithWindows(bool enabled) {
    const wchar_t* runKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    const wchar_t* valueName = L"SpacemouseHOTAS";
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, runKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (enabled) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring command = L"\"" + std::wstring(exe) + L"\"";
        RegSetValueExW(key, valueName, 0, REG_SZ, (const BYTE*)command.c_str(),
                       (DWORD)((command.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, valueName);
    }
    RegCloseKey(key);
}
