// Platform-independent application: settings, mapping, output, capture rules and the UI.
// Platforms feed it input events, call update() on events and timers, and draw its canvas.
#pragma once
#include <string>
#include <vector>
#include "core/capture_apps.h"
#include "core/mapping.h"
#include "core/spacemouse.h"
#include "platform/platform.h"
#include "ui/canvas.h"
#include "ui/widgets.h"

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

class App {
public:
    static const int kWidth = 960, kHeight = 600;       // logical window size
    static constexpr double kProcessPollSeconds = 5.0;  // only while a capture app runs, to notice it exit

    Profile profile;
    Mapper mapper;
    SpaceMouseState device;
    std::vector<CaptureApp> captureApps;
    VirtualPad* pad = nullptr;
    PlatformHooks hooks;

    bool firstRun = false;     // no config file existed
    bool configDirty = false;  // unsaved changes
    bool quitRequested = false;
    bool redrawNeeded = true;         // live data changed since the last frame
    bool timedUpdatesNeeded = false;  // call update() about every 10 ms while set
    bool ignoreRawReports = false;    // set while input arrives through the capture path

    void init(VirtualPad* virtualPad, const PlatformHooks& platformHooks, const std::string& configPath);
    void shutdown();

    void onDeviceReport(const uint8_t* data, int length);
    void setDeviceState(const int16_t* axes, uint32_t buttons);  // input from the capture path
    void onDeviceArrived(uint16_t vendorId, uint16_t productId, const char* name);
    void onDeviceRemoved();
    void update(double now);

    // Capture apps are detected when the foreground app changes; polling only watches for their exit.
    bool processPollingNeeded() const;
    void onForegroundAppChanged();
    void pollProcesses();
    bool captureActive() const { return captureApplied_; }

    void setOutputEnabled(bool enabled);
    void setForceCapture(bool enabled);
    void save();
    void revert();
    void reconnectPad();
    // Work that may block (file dialog, driver setup). Call outside of painting.
    bool hasDeferredActions() const { return browseRequested_ || padConfigureRequested_; }
    void runDeferredActions();

    // Draws one frame. Returns true if another frame should follow right away.
    bool drawFrame(const UiInput& frameInput, int scale);
    void releaseCanvas() { canvas_.release(); }
    const Canvas& canvas() const { return canvas_; }
    const std::string& configPath() const { return configPath_; }

private:
    Canvas canvas_;
    Ui ui_;
    std::string configPath_;

    int tab_ = 0;
    int selectedAxis_ = 0;
    int capturingButton_ = -1;                // button row waiting for a key or mouse press
    int buttonTargets_[kDeviceButtons] = {};  // menu index per button, synced with the profile
    std::string appEntryText_;
    std::string lastProcessQuery_;
    std::vector<std::string> processNames_;
    std::vector<std::string> suggestions_;
    int suggestionHighlight_ = -1;
    int appListScroll_ = 0;
    bool browseRequested_ = false;
    bool padConfigureRequested_ = false;

    std::string runningCaptureApp_;  // name of the matched running app, empty if none
    bool captureApplied_ = false;
    std::string captureError_;

    double lastUpdate_ = -1;
    bool outputSent_ = false;
    int32_t sentAxes_[kPadAxes] = {};
    uint32_t sentButtons_ = 0;
    int sentHat_ = -1;
    uint32_t sentKeys_[8] = {};
    uint32_t sentMouseButtons_ = 0;

    bool captureAppWatchActive() const;
    void applyCapture();
    void sendOutputs(bool force);
    void sendKeysAndMouse(bool releaseAll);
    void addCaptureApp(const CaptureApp& app);
    void refreshSuggestions();
    bool consumeButtonCapture(UiInput& frameInput);

    void drawHeader();
    void drawInputPanel(int x, int y, int w, int h);
    void drawCaptureSection(int x, int y, int w, int bottom);
    void drawOutputPanel(int x, int y, int w, int h);
    void drawEditor(int x, int y, int w, int h);
    void drawAxesTab(int x, int y, int w);
    void drawButtonsTab(int x, int y, int w);
    void drawInputsTab(int x, int y, int w);
    void drawSetupTab(int x, int y, int w, int h);
};
