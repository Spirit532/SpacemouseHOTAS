// Feeder for the signed vJoy 2.2.x kernel driver (BrunnerInnovation fork).
// Uses the driver's raw device interface and the same IOCTL as vJoyInterface.dll, so no vJoy DLLs are needed.
#include "platform/win32/vjoy_pad.h"
#include <windows.h>
#include <setupapi.h>
#include <shellapi.h>
#include <winioctl.h>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

namespace {

// {781EF630-72B2-11d2-B852-00C04FAD5101}: vJoy raw device interface
const GUID kVJoyInterface = {0x781EF630, 0x72B2, 0x11d2, {0xB8, 0x52, 0x00, 0xC0, 0x4F, 0xAD, 0x51, 0x01}};
const DWORD kIoctlLoadPositions = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x910, METHOD_BUFFERED, FILE_WRITE_ACCESS);
const DWORD kIoctlGetDeviceInfo = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x917, METHOD_BUFFERED, FILE_ANY_ACCESS);
const wchar_t* kDriverParametersKey = L"SYSTEM\\CurrentControlSet\\services\\vjoy\\Parameters";
const wchar_t* kHardwareIdPrefix = L"root\\VID_1234&PID_BEAD";

// JOYSTICK_POSITION_V3 from vJoy public.h (API version 3, default packing).
struct JoystickPosition {
    BYTE bDevice;
    LONG wThrottle, wRudder, wAileron;
    LONG wAxisX, wAxisY, wAxisZ, wAxisXRot, wAxisYRot, wAxisZRot, wSlider, wDial;
    LONG wWheel, wAccelerator, wBrake, wClutch, wSteering;
    LONG wAxisVX, wAxisVY;
    LONG lButtons;
    DWORD bHats, bHatsEx1, bHatsEx2, bHatsEx3;
    LONG lButtonsEx1, lButtonsEx2, lButtonsEx3;
    LONG wAxisVZ, wAxisVBRX, wAxisVBRY, wAxisVBRZ;
};
static_assert(sizeof(JoystickPosition) == 124, "vJoy position struct layout mismatch");

struct DeviceInfo {
    BYTE deviceId, implementedCount, isImplemented, maxDevices, driverFfb, deviceFfb;
};

// HID descriptor in vJoyConf's fixed layout, which the driver fills reports into:
// 16 x 32-bit axis slots (first 8 used), 4 x 32-bit hat slots (1 continuous POV), 128 button bits (32 used).
std::vector<BYTE> buildDescriptor(int deviceId) {
    std::vector<BYTE> d = {0x05, 0x01, 0x15, 0x00, 0x09, 0x04, 0xA1, 0x01, 0x05, 0x01, 0x85, (BYTE)deviceId, 0x09, 0x01,
                           0x15, 0x00, 0x26, 0xFF, 0x7F, 0x75, 0x20, 0x95, 0x01, 0xA1, 0x00};
    for (int slot = 0; slot < 16; slot++) {
        if (slot < 8)
            d.insert(d.end(), {0x09, (BYTE)(0x30 + slot), 0x81, 0x02});  // X..Dial
        else
            d.insert(d.end(), {0x81, 0x01});  // unused slot
    }
    d.push_back(0xC0);
    const BYTE hat[] = {0x15, 0x00, 0x27, 0x3C, 0x8C, 0x00, 0x00, 0x35, 0x00, 0x47, 0x3C, 0x8C, 0x00, 0x00,
                        0x65, 0x14, 0x75, 0x20, 0x95, 0x01, 0x09, 0x39, 0x81, 0x02, 0x95, 0x03, 0x81, 0x01};
    const BYTE buttons[] = {0x05, 0x09, 0x15, 0x00, 0x25, 0x01, 0x55, 0x00, 0x65, 0x00, 0x19, 0x01, 0x29, 0x20,
                            0x75, 0x01, 0x95, 0x20, 0x81, 0x02, 0x75, 0x60, 0x95, 0x01, 0x81, 0x01, 0xC0};
    d.insert(d.end(), hat, hat + sizeof hat);
    d.insert(d.end(), buttons, buttons + sizeof buttons);
    return d;
}

std::wstring deviceKeyPath(int deviceId) {
    wchar_t key[160];
    swprintf(key, 160, L"%ls\\Device%02d", kDriverParametersKey, deviceId);
    return key;
}

std::vector<BYTE> readInstalledDescriptor(int deviceId) {
    std::vector<BYTE> data(1024);
    DWORD size = (DWORD)data.size();
    if (RegGetValueW(HKEY_LOCAL_MACHINE, deviceKeyPath(deviceId).c_str(), L"HidReportDescriptor", RRF_RT_REG_BINARY,
                     nullptr, data.data(), &size) != ERROR_SUCCESS)
        size = 0;
    data.resize(size);
    return data;
}

// Device paths are "<namespace>\device_NNN"; the namespace comes from any vJoy interface path.
bool findInterfaceNamespace(std::wstring& prefix) {
    HDEVINFO set = SetupDiGetClassDevsW(&kVJoyInterface, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return false;
    SP_DEVICE_INTERFACE_DATA iface = {sizeof iface};
    bool found = false;
    if (SetupDiEnumDeviceInterfaces(set, nullptr, &kVJoyInterface, 0, &iface)) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &needed, nullptr);
        std::vector<BYTE> buffer(needed + 16);
        auto* detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)buffer.data();
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, needed, nullptr, nullptr)) {
            if (const wchar_t* slash = wcsrchr(detail->DevicePath, L'\\')) {
                prefix.assign(detail->DevicePath, (size_t)(slash + 1 - detail->DevicePath));
                found = true;
            }
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

class VJoyPad : public VirtualPad {
public:
    ~VJoyPad() override {
        close();
        if (event_) CloseHandle(event_);
    }

    bool open(int deviceId) override {
        close();
        deviceId_ = deviceId;
        layoutInstalled_ = readInstalledDescriptor(deviceId) == buildDescriptor(deviceId);
        std::wstring prefix;
        if (!findInterfaceNamespace(prefix)) {
            snprintf(status_, sizeof status_, "vJoy driver not found");
            return false;
        }
        wchar_t path[512];
        swprintf(path, 512, L"%lsdevice_%03d", prefix.c_str(), deviceId);
        handle_ = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            DWORD error = GetLastError();
            if (error == ERROR_ACCESS_DENIED)
                snprintf(status_, sizeof status_, "vJoy #%d busy (other feeder)", deviceId);
            else
                snprintf(status_, sizeof status_, "vJoy #%d not configured (error %lu)", deviceId, error);
            return false;
        }
        if (!event_) event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DeviceInfo info = {};
        if (!ioctl(kIoctlGetDeviceInfo, nullptr, 0, &info, sizeof info) || info.deviceId != deviceId) {
            close();
            snprintf(status_, sizeof status_, "vJoy #%d not responding", deviceId);
            return false;
        }
        snprintf(status_, sizeof status_, layoutInstalled_ ? "vJoy #%d ready" : "vJoy #%d ready, layout not set",
                 deviceId);
        return true;
    }

    void close() override {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
        snprintf(status_, sizeof status_, "vJoy closed");
    }

    bool isOpen() const override { return handle_ != INVALID_HANDLE_VALUE; }
    bool needsConfiguration() const override { return !layoutInstalled_; }

    bool send(const float* axes, uint32_t buttons, int hat) override {
        if (handle_ == INVALID_HANDLE_VALUE) return false;
        JoystickPosition p = {};
        p.bDevice = (BYTE)deviceId_;
        LONG* slots[kPadAxes] = {&p.wAxisX,    &p.wAxisY,    &p.wAxisZ,  &p.wAxisXRot,
                                 &p.wAxisYRot, &p.wAxisZRot, &p.wSlider, &p.wDial};
        for (int i = 0; i < kPadAxes; i++) {
            float v = axes[i] < -1 ? -1 : (axes[i] > 1 ? 1 : axes[i]);
            *slots[i] = (LONG)lroundf((v + 1.0f) * 0.5f * 32767.0f);  // driver range 0..32767
        }
        const LONG center = 16384;
        p.wThrottle = p.wRudder = p.wAileron = p.wWheel = p.wAccelerator = p.wBrake = p.wClutch = p.wSteering = center;
        p.wAxisVX = p.wAxisVY = p.wAxisVZ = p.wAxisVBRX = p.wAxisVBRY = p.wAxisVBRZ = center;
        p.lButtons = (LONG)buttons;
        p.bHats = hat < 0 ? 0xFFFFFFFFu : (DWORD)(hat * 4500);  // hundredths of a degree
        p.bHatsEx1 = p.bHatsEx2 = p.bHatsEx3 = 0xFFFFFFFFu;
        if (ioctl(kIoctlLoadPositions, &p, sizeof p, nullptr, 0)) return true;
        DWORD error = GetLastError();
        close();
        snprintf(status_, sizeof status_, "vJoy #%d write failed (error %lu)", deviceId_, error);
        return false;
    }

    const char* status() const override { return status_; }

    const char* help() const override { return "Configuring vJoy sets 8 axes, 32 buttons, 1 hat."; }

    const char* configureLabel() const override { return "Configure vJoy"; }

    // Writing the driver's registry key needs admin, so relaunch this exe elevated for that step.
    void configure() override {
        close();
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        wchar_t args[64];
        swprintf(args, 64, L"--configure-vjoy %d", deviceId_);
        SHELLEXECUTEINFOW info = {sizeof info};
        info.fMask = SEE_MASK_NOCLOSEPROCESS;
        info.lpVerb = L"runas";
        info.lpFile = exe;
        info.lpParameters = args;
        info.nShow = SW_HIDE;
        if (ShellExecuteExW(&info) && info.hProcess) {
            WaitForSingleObject(info.hProcess, 30000);
            CloseHandle(info.hProcess);
        }
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    HANDLE event_ = nullptr;
    int deviceId_ = 1;
    bool layoutInstalled_ = true;
    char status_[96] = "vJoy not opened";

    // Overlapped DeviceIoControl with a timeout, as vJoyInterface.dll does.
    bool ioctl(DWORD code, void* in, DWORD inSize, void* out, DWORD outSize) {
        OVERLAPPED overlapped = {};
        overlapped.hEvent = event_;
        ResetEvent(event_);
        DWORD bytes = 0;
        if (DeviceIoControl(handle_, code, in, inSize, out, outSize, &bytes, &overlapped)) return true;
        if (GetLastError() != ERROR_IO_PENDING) return false;
        if (WaitForSingleObject(event_, 500) != WAIT_OBJECT_0) {
            CancelIoEx(handle_, &overlapped);
            GetOverlappedResult(handle_, &overlapped, &bytes, TRUE);
            SetLastError(ERROR_TIMEOUT);
            return false;
        }
        return GetOverlappedResult(handle_, &overlapped, &bytes, FALSE) != FALSE;
    }
};

// Disable and re-enable the vJoy root device so the driver reads the new descriptor.
bool restartVJoyDevice() {
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return false;
    bool restarted = false;
    SP_DEVINFO_DATA device = {sizeof device};
    size_t prefixLength = wcslen(kHardwareIdPrefix);
    for (DWORD i = 0; !restarted && SetupDiEnumDeviceInfo(set, i, &device); i++) {
        wchar_t ids[1024] = {};
        if (!SetupDiGetDeviceRegistryPropertyW(set, &device, SPDRP_HARDWAREID, nullptr, (BYTE*)ids, sizeof ids - 4,
                                               nullptr))
            continue;
        for (const wchar_t* id = ids; *id; id += wcslen(id) + 1) {
            if (_wcsnicmp(id, kHardwareIdPrefix, prefixLength) != 0) continue;
            SP_PROPCHANGE_PARAMS change = {};
            change.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
            change.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
            change.Scope = DICS_FLAG_CONFIGSPECIFIC;
            change.StateChange = DICS_DISABLE;
            if (SetupDiSetClassInstallParamsW(set, &device, &change.ClassInstallHeader, sizeof change))
                SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &device);
            change.StateChange = DICS_ENABLE;
            restarted = SetupDiSetClassInstallParamsW(set, &device, &change.ClassInstallHeader, sizeof change) &&
                        SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &device);
            break;
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return restarted;
}

}  // namespace

VirtualPad* createVJoyPad() { return new VJoyPad(); }

bool isVJoyInstalled() {
    std::wstring prefix;
    return findInterfaceNamespace(prefix);
}

int configureVJoyElevated(int deviceId) {
    if (deviceId < 1 || deviceId > 16) return 2;
    std::vector<BYTE> descriptor = buildDescriptor(deviceId);
    HKEY key;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, deviceKeyPath(deviceId).c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                        &key, nullptr) != ERROR_SUCCESS)
        return 3;
    DWORD size = (DWORD)descriptor.size();
    LONG written = RegSetValueExW(key, L"HidReportDescriptor", 0, REG_BINARY, descriptor.data(), size);
    LONG sizeWritten = RegSetValueExW(key, L"HidReportDescriptorSize", 0, REG_DWORD, (const BYTE*)&size, sizeof size);
    RegCloseKey(key);
    if (written != ERROR_SUCCESS || sizeWritten != ERROR_SUCCESS) return 4;
    return restartVJoyDevice() ? 0 : 5;
}
