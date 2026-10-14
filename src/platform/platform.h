// What each platform provides to the shared App.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

constexpr int kPadAxes = 8;
constexpr int kPadButtons = 32;

// Virtual joystick output device.
class VirtualPad {
public:
    virtual ~VirtualPad() {}
    virtual bool open(int deviceId) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    // axes -1..1 in X Y Z RX RY RZ SL0 SL1 order, bit 0 = button 1, hat -1 or 0..7 (N, NE, ... NW).
    virtual bool send(const float* axes, uint32_t buttons, int hat) = 0;
    virtual const char* status() const = 0;                         // one short line
    virtual const char* help() const = 0;                           // setup hint, '\n' separates lines
    virtual const char* configureLabel() const { return nullptr; }  // button text if configure() exists
    virtual void configure() {}  // may block (elevation prompt); never called while painting
    virtual bool needsConfiguration() const { return false; }  // device works only after configure()
};

// Optional services. A null member means the platform does not support it and its UI is hidden.
struct PlatformHooks {
    void (*injectKey)(int vk, bool down) = nullptr;
    void (*injectMouseButton)(int button, bool down) = nullptr;
    // Grab (true) or release the 3D mouse through the vendor driver. Returns nullptr or an error line.
    const char* (*setDeviceCapture)(bool capture) = nullptr;
    void (*listProcesses)(std::vector<std::string>& executableNames) = nullptr;
    bool (*browseForExecutable)(std::string& path) = nullptr;  // false if cancelled
    void (*setStartWithSystem)(bool enabled) = nullptr;
    const char* startWithSystemLabel = nullptr;
};
