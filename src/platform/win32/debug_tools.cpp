#include "platform/win32/debug_tools.h"
#include <windows.h>
#include <mmsystem.h>
#include <cstdio>
#include <vector>
#include "app.h"
#include "platform/win32/system_win32.h"
#include "platform/win32/vjoy_pad.h"
#include "ui/icon.h"

// Stand-in output device for screenshots: open, but not configured yet.
class PreviewPad : public VirtualPad {
public:
    bool open(int) override { return true; }
    void close() override {}
    bool isOpen() const override { return true; }
    bool send(const float*, uint32_t, int) override { return true; }
    const char* status() const override { return "vJoy #1 ready, layout not set"; }
    const char* help() const override { return ""; }
    const char* configureLabel() const override { return "Configure vJoy"; }
    bool needsConfiguration() const override { return true; }
};

int runScreenshot(const wchar_t* bmpPath, int scale, wchar_t** steps, int stepCount, const std::string& configPath) {
    // Stub capture hook so the capture section renders; process listing is real.
    PlatformHooks hooks;
    hooks.setDeviceCapture = [](bool) -> const char* { return nullptr; };
    hooks.listProcesses = listProcesses;
    PreviewPad pad;
    App app;
    app.init(&pad, hooks, configPath);
    UiInput in;
    app.drawFrame(in, scale);
    for (int i = 0; i < stepCount; i++) {
        int x = 0, y = 0;
        in = UiInput();
        if (swscanf(steps[i], L"h:%d,%d", &x, &y) == 2) {
            in.mouseX = x;
            in.mouseY = y;
        } else if (swscanf(steps[i], L"k:%d", &x) == 1) {
            in.key = x;
        } else if (!wcsncmp(steps[i], L"t:", 2)) {
            in.text = toUtf8(steps[i] + 2);
        } else if (swscanf(steps[i], L"%d,%d", &x, &y) == 2) {
            in.mouseX = x;
            in.mouseY = y;
            in.leftPressed = in.leftDown = true;
            app.drawFrame(in, scale);
            in.leftPressed = in.leftDown = false;
            in.leftReleased = true;
        }
        app.drawFrame(in, scale);
    }
    in.clearEdges();
    app.drawFrame(in, scale);

    const Canvas& canvas = app.canvas();
    FILE* f = _wfopen(bmpPath, L"wb");
    if (!f) return 1;
    int rowBytes = canvas.width * 3;
    BITMAPFILEHEADER fileHeader = {
        0x4D42, (DWORD)(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + rowBytes * canvas.height), 0, 0,
        sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)};
    BITMAPINFOHEADER infoHeader = {sizeof infoHeader, canvas.width, -canvas.height, 1, 24, BI_RGB};
    fwrite(&fileHeader, sizeof fileHeader, 1, f);
    fwrite(&infoHeader, sizeof infoHeader, 1, f);
    std::vector<BYTE> row(rowBytes);
    for (int y = 0; y < canvas.height; y++) {
        for (int x = 0; x < canvas.width; x++) {
            uint32_t c = canvas.pixels[y * canvas.width + x];
            row[x * 3] = c & 255;
            row[x * 3 + 1] = (c >> 8) & 255;
            row[x * 3 + 2] = (c >> 16) & 255;
        }
        fwrite(row.data(), 1, rowBytes, f);
    }
    fclose(f);
    return 0;
}

int runVJoySelfTest(const wchar_t* reportPath) {
    FILE* f = _wfopen(reportPath, L"w");
    if (!f) return 1;
    setvbuf(f, nullptr, _IONBF, 0);
    VirtualPad* pad = createVJoyPad();
    bool opened = pad->open(1);
    fprintf(f, "open=%d status=%s\n", opened, pad->status());

    typedef UINT(WINAPI * GetDeviceCountFn)();
    typedef UINT(WINAPI * GetCapsFn)(UINT_PTR, JOYCAPSW*, UINT);
    typedef UINT(WINAPI * GetPositionFn)(UINT, JOYINFOEX*);
    HMODULE winmm = LoadLibraryW(L"winmm.dll");
    auto getDeviceCount = (GetDeviceCountFn)GetProcAddress(winmm, "joyGetNumDevs");
    auto getCaps = (GetCapsFn)GetProcAddress(winmm, "joyGetDevCapsW");
    auto getPosition = (GetPositionFn)GetProcAddress(winmm, "joyGetPosEx");
    int joystick = -1;
    for (UINT i = 0; getDeviceCount && i < getDeviceCount(); i++) {
        JOYCAPSW caps = {};
        if (getCaps(i, &caps, sizeof caps) == 0 && caps.wMid == 0x1234 && caps.wPid == 0xBEAD) {
            fprintf(f, "joystick %u: axes=%u buttons=%u pov=%d range=%u..%u\n", i, caps.wNumAxes, caps.wNumButtons,
                    (caps.wCaps & JOYCAPS_HASPOV) != 0, caps.wXmin, caps.wXmax);
            joystick = (int)i;
            break;
        }
    }
    const float axes[3][kPadAxes] = {{0.5f, -0.5f, 1.0f, -1.0f, 0, 0.25f, 0, 0}, {-1, 1, 0, 0.5f, 0, 0, 0, 0}, {}};
    const uint32_t buttons[3] = {0x5, 0x80000001u, 0};
    const int hats[3] = {2, 0, -1};
    for (int t = 0; t < 3; t++) {
        bool sent = pad->send(axes[t], buttons[t], hats[t]);
        Sleep(150);
        JOYINFOEX position = {sizeof position, JOY_RETURNALL};
        UINT result = joystick >= 0 ? getPosition((UINT)joystick, &position) : 99;
        fprintf(f, "test%d sent=%d read=%u X=%lu Y=%lu Z=%lu R=%lu U=%lu V=%lu buttons=%08lX pov=%lu\n", t, sent,
                result, position.dwXpos, position.dwYpos, position.dwZpos, position.dwRpos, position.dwUpos,
                position.dwVpos, position.dwButtons, position.dwPOV);
    }
    pad->close();
    delete pad;
    fclose(f);
    return 0;
}

int writeIconFile(const wchar_t* icoPath) {
    const int sizes[] = {16, 24, 32, 48, 64};
    const int count = sizeof sizes / sizeof sizes[0];
    std::vector<std::vector<BYTE>> images;
    for (int size : sizes) {
        std::vector<uint32_t> argb(size * size);
        makeIcon(argb.data(), size, true);
        BITMAPINFOHEADER header = {sizeof header, size, size * 2, 1, 32, BI_RGB};  // height covers XOR + AND masks
        std::vector<BYTE> image((BYTE*)&header, (BYTE*)&header + sizeof header);
        for (int y = size - 1; y >= 0; y--)  // bottom-up
            image.insert(image.end(), (BYTE*)&argb[y * size], (BYTE*)&argb[y * size] + size * 4);
        int maskRow = ((size + 31) / 32) * 4;
        image.insert(image.end(), (size_t)maskRow * size, 0);  // alpha channel does the masking
        images.push_back(image);
    }
    FILE* f = _wfopen(icoPath, L"wb");
    if (!f) return 1;
    const WORD fileHeader[3] = {0, 1, (WORD)count};
    fwrite(fileHeader, sizeof fileHeader, 1, f);
    DWORD offset = 6 + 16 * count;
    for (int i = 0; i < count; i++) {
        BYTE entry[16] = {(BYTE)sizes[i], (BYTE)sizes[i], 0, 0, 1, 0, 32, 0};
        DWORD bytes = (DWORD)images[i].size();
        memcpy(entry + 8, &bytes, 4);
        memcpy(entry + 12, &offset, 4);
        fwrite(entry, sizeof entry, 1, f);
        offset += bytes;
    }
    for (const std::vector<BYTE>& image : images) fwrite(image.data(), 1, image.size(), f);
    fclose(f);
    return 0;
}
