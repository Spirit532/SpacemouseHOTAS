#include "app.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "core/config.h"
#include "core/keys.h"

using namespace theme;

namespace {

const uint32_t kGroupColors[kAxisGroups + 1] = {0, 0x4CC979, 0x3CC8D8, 0xD86ED8, 0xE0C84C, 0x9C7CFF};

uint32_t groupColor(int group) { return group > 0 && group <= kAxisGroups ? kGroupColors[group] : 0; }

}  // namespace

// Lifecycle ----------------------------------------------------------------------------------

void App::init(VirtualPad* virtualPad, const PlatformHooks& platformHooks, const std::string& path) {
    pad = virtualPad;
    hooks = platformHooks;
    configPath_ = path;
    setDefaultProfile(profile);
    captureApps = defaultCaptureApps();
    firstRun = !loadConfig(configPath_.c_str(), profile, captureApps);
    if (firstRun) configDirty = true;
    if (hooks.setStartWithSystem) hooks.setStartWithSystem(profile.startWithSystem);
    if (pad) pad->open(profile.vjoyDevice);
    if (captureAppWatchActive()) pollProcesses();
    applyCapture();
    sendOutputs(true);
}

void App::shutdown() {
    if (configDirty) save();
    sendKeysAndMouse(true);
    if (hooks.setDeviceCapture && captureApplied_) hooks.setDeviceCapture(false);
    captureApplied_ = false;
    if (pad) {
        const float centered[kPadAxes] = {};
        if (pad->isOpen()) pad->send(centered, 0, -1);
        pad->close();
    }
    canvas_.release();
}

void App::save() {
    if (saveConfig(configPath_.c_str(), profile, captureApps)) configDirty = false;
}

void App::revert() {
    Profile before = profile;
    setDefaultProfile(profile);
    captureApps = defaultCaptureApps();
    loadConfig(configPath_.c_str(), profile, captureApps);
    configDirty = false;
    if (profile.vjoyDevice != before.vjoyDevice) reconnectPad();
    if (profile.startWithSystem != before.startWithSystem && hooks.setStartWithSystem)
        hooks.setStartWithSystem(profile.startWithSystem);
    applyCapture();
}

void App::reconnectPad() {
    if (!pad) return;
    pad->close();
    pad->open(profile.vjoyDevice);
    sendOutputs(true);
    redrawNeeded = true;
}

void App::runDeferredActions() {
    if (browseRequested_) {
        browseRequested_ = false;
        std::string path;
        if (hooks.browseForExecutable && hooks.browseForExecutable(path)) addCaptureApp(captureAppFromExecutable(path));
    }
    if (padConfigureRequested_) {
        padConfigureRequested_ = false;
        if (pad) {
            pad->configure();
            reconnectPad();
        }
    }
}

// Input --------------------------------------------------------------------------------------

void App::onDeviceReport(const uint8_t* data, int length) {
    if (ignoreRawReports || !parseSpaceMouseReport(data, length, device)) return;
    for (int i = 0; i < INPUT_AXIS_COUNT; i++) mapper.rawInput[i] = device.axis[i];
    mapper.deviceButtons = device.buttons;
}

void App::setDeviceState(const int16_t* axes, uint32_t buttons) {
    for (int i = 0; i < INPUT_AXIS_COUNT; i++) {
        device.axis[i] = axes[i];
        mapper.rawInput[i] = axes[i];
    }
    device.buttons = buttons;
    mapper.deviceButtons = buttons;
}

void App::onDeviceArrived(uint16_t vendorId, uint16_t productId, const char* name) {
    device.connected++;
    device.vid = vendorId;
    device.pid = productId;
    snprintf(device.name, sizeof device.name, "%s", name && *name ? name : spaceMouseModelName(vendorId, productId));
    if (captureApplied_ && hooks.setDeviceCapture) {  // grab the new device as well
        const char* error = hooks.setDeviceCapture(true);
        captureError_ = error ? error : "";
    }
    redrawNeeded = true;
}

void App::onDeviceRemoved() {
    if (device.connected > 0) device.connected--;
    if (device.connected == 0) {
        for (int16_t& a : device.axis) a = 0;
        device.buttons = 0;
        for (float& r : mapper.rawInput) r = 0;
        mapper.deviceButtons = 0;
    }
    redrawNeeded = true;
}

void App::update(double now) {
    float dt = lastUpdate_ < 0 ? 0.0f : (float)(now - lastUpdate_);
    if (dt < 0) dt = 0;
    if (dt > 0.1f) dt = 0.1f;  // after sleep or long stalls
    lastUpdate_ = now;
    timedUpdatesNeeded = mapper.update(profile, dt, profile.outputEnabled);
    if (mapper.toggleOutputRequested) {
        mapper.toggleOutputRequested = false;
        setOutputEnabled(!profile.outputEnabled);
    }
    sendOutputs(false);
    redrawNeeded = true;
}

// Output -------------------------------------------------------------------------------------

void App::setOutputEnabled(bool enabled) {
    if (profile.outputEnabled == enabled) return;
    profile.outputEnabled = enabled;
    configDirty = true;
    sendOutputs(true);
    redrawNeeded = true;
}

void App::sendKeysAndMouse(bool releaseAll) {
    bool active = profile.outputEnabled && !releaseAll;
    if (hooks.injectKey) {
        for (int word = 0; word < 8; word++) {
            uint32_t held = active ? mapper.keys[word] : 0;
            uint32_t changedBits = held ^ sentKeys_[word];
            for (int bit = 0; changedBits; bit++, changedBits >>= 1)
                if (changedBits & 1u) hooks.injectKey(word * 32 + bit, (held >> bit) & 1u);
            sentKeys_[word] = held;
        }
    }
    if (hooks.injectMouseButton) {
        uint32_t held = active ? mapper.mouseButtons : 0;
        uint32_t changedBits = held ^ sentMouseButtons_;
        for (int bit = 0; changedBits; bit++, changedBits >>= 1)
            if (changedBits & 1u) hooks.injectMouseButton(bit, (held >> bit) & 1u);
        sentMouseButtons_ = held;
    }
}

// Only talks to the device when the quantized state changed.
void App::sendOutputs(bool force) {
    sendKeysAndMouse(false);
    if (!pad || !pad->isOpen()) {
        outputSent_ = false;
        return;
    }
    bool active = profile.outputEnabled;
    float axes[kPadAxes];
    int32_t quantized[kPadAxes];
    for (int i = 0; i < kPadAxes; i++) {
        axes[i] = active ? mapper.output[i] : 0.0f;
        quantized[i] = (int32_t)lroundf(axes[i] * 32767.0f);
    }
    uint32_t buttons = active ? mapper.joystickButtons : 0;
    int hat = active ? mapper.hat : -1;
    bool same =
        outputSent_ && !memcmp(quantized, sentAxes_, sizeof quantized) && buttons == sentButtons_ && hat == sentHat_;
    if (same && !force) return;
    outputSent_ = pad->send(axes, buttons, hat);
    if (outputSent_) {
        memcpy(sentAxes_, quantized, sizeof quantized);
        sentButtons_ = buttons;
        sentHat_ = hat;
    }
}

// Capture ------------------------------------------------------------------------------------

bool App::processPollingNeeded() const { return captureAppWatchActive() && !runningCaptureApp_.empty(); }

bool App::captureAppWatchActive() const {
    return hooks.setDeviceCapture && hooks.listProcesses && profile.captureWhenRunning && !captureApps.empty();
}

void App::onForegroundAppChanged() {
    if (captureAppWatchActive()) pollProcesses();
}

void App::pollProcesses() {
    if (!hooks.listProcesses) return;
    processNames_.clear();
    hooks.listProcesses(processNames_);
    const CaptureApp* running = findRunningCaptureApp(captureApps, processNames_);
    std::string name = running ? running->name : "";
    if (name != runningCaptureApp_) {
        runningCaptureApp_ = name;
        redrawNeeded = true;
    }
    applyCapture();
}

void App::setForceCapture(bool enabled) {
    if (profile.forceCapture == enabled) return;
    profile.forceCapture = enabled;
    configDirty = true;
    applyCapture();
}

void App::applyCapture() {
    if (!hooks.setDeviceCapture) return;
    bool appTriggered = profile.captureWhenRunning && !runningCaptureApp_.empty();
    bool wanted = profile.forceCapture || appTriggered;
    if (wanted == captureApplied_) return;
    const char* error = hooks.setDeviceCapture(wanted);
    captureError_ = error ? error : "";
    captureApplied_ = wanted && !error;
    redrawNeeded = true;
}

void App::addCaptureApp(const CaptureApp& app) {
    if (app.name.empty() || app.match.empty()) return;
    for (const CaptureApp& existing : captureApps)
        if (toLower(existing.match) == toLower(app.match)) return;
    captureApps.push_back(app);
    configDirty = true;
    if (captureAppWatchActive()) pollProcesses();
}

void App::refreshSuggestions() {
    if (appEntryText_ != lastProcessQuery_) {
        lastProcessQuery_ = appEntryText_;
        if (hooks.listProcesses) {
            processNames_.clear();
            hooks.listProcesses(processNames_);
        }
        suggestionHighlight_ = -1;
    }
    suggestions_.clear();
    if (appEntryText_.empty()) return;
    std::string query = toLower(appEntryText_);
    for (const std::string& process : processNames_)
        if (toLower(process).find(query) != std::string::npos &&
            std::find(suggestions_.begin(), suggestions_.end(), process) == suggestions_.end())
            suggestions_.push_back(process);
    std::sort(suggestions_.begin(), suggestions_.end());
    if (suggestions_.size() > 40) suggestions_.resize(40);
    if (suggestionHighlight_ >= (int)suggestions_.size()) suggestionHighlight_ = -1;
}

// Drawing helpers ----------------------------------------------------------------------------

static void drawBar(Canvas& c, int x, int y, int w, int h, float value, uint32_t color, bool active = true) {
    c.fillRect(x, y, w, h, field);
    c.strokeRect(x, y, w, h, border);
    int mid = x + w / 2;
    int length = (int)lroundf(value * (w / 2 - 1));
    if (length > 0)
        c.fillRect(mid, y + 1, length, h - 2, active ? color : faint);
    else if (length < 0)
        c.fillRect(mid + length, y + 1, -length, h - 2, active ? color : faint);
    c.fillRect(mid, y, 1, h, dim);
}

// 2D crosshair box; +vy is drawn downward.
static void drawCrosshairBox(Canvas& c, int x, int y, int size, float vx, float vy, uint32_t color, const char* title) {
    c.text(x, y - 12, title, dim);
    c.fillRect(x, y, size, size, field);
    c.strokeRect(x, y, size, size, border);
    uint32_t gridColor = blendColor(field, border, 128);
    for (int i = 1; i < 4; i += 2) {
        c.fillRect(x + i * size / 4, y + 1, 1, size - 2, gridColor);
        c.fillRect(x + 1, y + i * size / 4, size - 2, 1, gridColor);
    }
    int cx = x + size / 2, cy = y + size / 2;
    c.fillRect(cx, y + 1, 1, size - 2, border);
    c.fillRect(x + 1, cy, size - 2, 1, border);
    int reach = size / 2 - 4;
    int px = cx + (int)lroundf(vx * reach), py = cy + (int)lroundf(vy * reach);
    c.line(cx, cy, px, py, blendColor(field, color, 140));
    c.fillRect(px - 6, py, 13, 1, color);
    c.fillRect(px, py - 6, 1, 13, color);
    c.disc(px, py, 2, color);
    char label[40];
    snprintf(label, sizeof label, "%+.2f %+.2f", vx, vy);
    c.text(x + 3, y + size - 11, label, dim);
}

static void drawButtonGrid(Canvas& c, int x, int y, uint32_t pressed, uint32_t color) {
    char label[4];
    for (int i = 0; i < 32; i++) {
        int cellX = x + (i % 16) * 18, cellY = y + (i / 16) * 16;
        bool on = (pressed >> i) & 1u;
        c.fillRect(cellX, cellY, 17, 14, on ? color : field);
        c.strokeRect(cellX, cellY, 17, 14, on ? color : border);
        snprintf(label, sizeof label, "%d", i + 1);
        int labelW = c.textWidth(label);
        c.text(cellX + (17 - labelW) / 2 + (labelW > 8 ? 1 : 0), cellY + 3, label, on ? background : dim);
    }
}

// Word-wrapped text; returns the y below the last line.
static int drawWrapped(Canvas& c, int x, int y, int maxWidth, const char* s, uint32_t color) {
    std::string line;
    auto flush = [&]() {
        if (!line.empty()) c.text(x, y, line.c_str(), color);
        y += 13;
        line.clear();
    };
    const char* p = s;
    while (*p) {
        if (*p == '\n') {
            flush();
            p++;
            continue;
        }
        const char* wordEnd = p;
        while (*wordEnd && *wordEnd != ' ' && *wordEnd != '\n') wordEnd++;
        std::string word(p, wordEnd);
        std::string candidate = line.empty() ? word : line + " " + word;
        if (!line.empty() && c.textWidth(candidate.c_str()) > maxWidth) {
            flush();
            line = word;
        } else {
            line = candidate;
        }
        while (c.textWidth(line.c_str()) > maxWidth && line.size() > 1) {  // a single word wider than the line
            size_t fit = line.size() - 1;
            while (fit > 1 && c.textWidth(line.substr(0, fit).c_str()) > maxWidth) fit--;
            std::string rest = line.substr(fit);
            line.resize(fit);
            flush();
            line = rest;
        }
        p = wordEnd;
        while (*p == ' ') p++;
    }
    if (!line.empty()) flush();
    return y;
}

static const char* modeItem(int v, void*) { return axisModeName(v); }
static const char* inputItem(int v, void*) { return inputAxisName(v); }
static const char* deadzoneItem(int v, void*) { return deadzoneShapeName(v); }
static const char* curveItem(int v, void*) { return curveName(v); }
static const char* groupItem(int v, void*) { return axisGroupName(v); }
static const char* targetItem(int v, void*) { return buttonTarget(v).name; }
static const char* numberItem(int v, void*) {
    static char text[16];
    snprintf(text, sizeof text, "%d", v);
    return text;
}
static const char* scaleItem(int v, void*) {
    static const char* const kNames[4] = {"Auto", "1x", "2x", "3x"};
    return kNames[v < 0 ? 0 : (v > 3 ? 3 : v)];
}

// Frame --------------------------------------------------------------------------------------

// While a button row waits for a binding, the next key or mouse press becomes its mapping and
// all input is withheld from the widgets.
bool App::consumeButtonCapture(UiInput& in) {
    if (capturingButton_ < 0) return false;
    ButtonMapping& mapping = profile.buttons[capturingButton_];
    bool done = true;
    if (in.key == VK_CODE_ESCAPE) {
    } else if (in.key == VK_CODE_DELETE) {
        mapping = {ACTION_NONE, 0};
    } else if (in.key && keyByVk(in.key)) {
        mapping = {ACTION_KEY, in.key};
    } else if (in.leftPressed) {
        mapping = {ACTION_MOUSE, MOUSE_LEFT};
    } else if (in.rightPressed) {
        mapping = {ACTION_MOUSE, MOUSE_RIGHT};
    } else if (in.middlePressed) {
        mapping = {ACTION_MOUSE, MOUSE_MIDDLE};
    } else if (in.x1Pressed) {
        mapping = {ACTION_MOUSE, MOUSE_X1};
    } else if (in.x2Pressed) {
        mapping = {ACTION_MOUSE, MOUSE_X2};
    } else {
        done = false;
    }
    if (done) capturingButton_ = -1;
    in.clearEdges();
    return true;
}

bool App::drawFrame(const UiInput& frameInput, int scale) {
    if (!canvas_.resize(kWidth, kHeight, scale)) return false;
    Profile before = profile;
    std::vector<CaptureApp> appsBefore = captureApps;
    UiInput in = frameInput;
    bool wasCapturing = consumeButtonCapture(in);
    for (int b = 0; b < kDeviceButtons; b++) buttonTargets_[b] = findButtonTarget(profile.buttons[b]);
    int targetsBefore[kDeviceButtons];
    memcpy(targetsBefore, buttonTargets_, sizeof buttonTargets_);

    ui_.begin(canvas_, in);
    canvas_.fillRect(0, 0, kWidth, kHeight, background);
    drawHeader();
    drawInputPanel(8, 38, 308, 554);
    drawOutputPanel(324, 38, 308, 554);
    drawEditor(640, 38, 312, 554);
    ui_.end();
    redrawNeeded = false;

    for (int b = 0; b < kDeviceButtons; b++)
        if (buttonTargets_[b] != targetsBefore[b]) {
            const ButtonTarget& t = buttonTarget(buttonTargets_[b]);
            profile.buttons[b] = {t.action, t.param};
        }
    const AxisConfig& edited = profile.axes[selectedAxis_];
    int group = before.axes[selectedAxis_].group;
    if (group > 0 && edited.group == group)
        for (int o = 0; o < OUTPUT_AXIS_COUNT; o++)
            if (o != selectedAxis_ && profile.axes[o].group == group)
                copyGroupedSettings(before.axes[selectedAxis_], edited, profile.axes[o]);

    bool profileChanged = memcmp(&before, &profile, sizeof profile) != 0;
    if (profileChanged || !(appsBefore == captureApps)) {
        configDirty = true;
        if (before.vjoyDevice != profile.vjoyDevice) reconnectPad();
        if (before.startWithSystem != profile.startWithSystem && hooks.setStartWithSystem)
            hooks.setStartWithSystem(profile.startWithSystem);
        if (before.captureWhenRunning != profile.captureWhenRunning || !(appsBefore == captureApps)) {
            if (captureAppWatchActive())
                pollProcesses();
            else
                runningCaptureApp_.clear();
        }
        applyCapture();
        mapper.update(profile, 0.0f, profile.outputEnabled);  // outputs follow the edit at once
        sendOutputs(false);
    }
    return ui_.changed || wasCapturing != (capturingButton_ >= 0);
}

void App::drawHeader() {
    Canvas& c = canvas_;
    c.fillRect(0, 0, kWidth, 30, panel);
    c.fillRect(0, 30, kWidth, 1, border);
    int x = c.text(12, 11, "SpaceMouse", output, true);
    x = c.text(x + 4, 11, "HOTAS", text, true);
    c.text(x + 6, 11, "v" APP_VERSION, faint);

    bool enabled = profile.outputEnabled;
    if (ui_.button(widgetId("header.output"), 176, 5, 116, 20, enabled ? "OUTPUT: ON" : "OUTPUT: OFF", true,
                   enabled ? good : bad))
        setOutputEnabled(!enabled);

    bool connected = device.connected > 0;
    c.disc(308, 15, 4, connected ? good : bad);
    c.textClipped(318, 11, 196, connected ? device.name : "No 3D mouse connected", connected ? text : dim);

    bool padOpen = pad && pad->isOpen();
    c.disc(528, 15, 4, padOpen ? good : bad);
    c.textClipped(538, 11, 210, pad ? pad->status() : "No output backend", padOpen ? text : dim);

    if (ui_.button(widgetId("header.save"), 756, 5, 60, 20, "Save", configDirty, warn)) save();
    if (ui_.button(widgetId("header.revert"), 820, 5, 60, 20, "Revert")) revert();
    if (ui_.button(widgetId("header.defaults"), 884, 5, 68, 20, "Defaults")) {
        Profile kept = profile;
        setDefaultProfile(profile);
        profile.vjoyDevice = kept.vjoyDevice;
        profile.uiScale = kept.uiScale;
        profile.openWindowAtStart = kept.openWindowAtStart;
        profile.startWithSystem = kept.startWithSystem;
        profile.outputEnabled = kept.outputEnabled;
        profile.forceCapture = kept.forceCapture;
        profile.captureWhenRunning = kept.captureWhenRunning;
    }
    ui_.hint("Mapping defaults. Keeps device, startup and capture settings.");
}

void App::drawInputPanel(int x, int y, int w, int h) {
    Canvas& c = canvas_;
    ui_.panel(x, y, w, h, "INPUT - 3D MOUSE", input);
    char line[96];
    int rowY = y + 26;
    for (int i = 0; i < INPUT_AXIS_COUNT; i++, rowY += 18) {
        c.text(x + 10, rowY + 2, inputAxisName(i), text);
        drawBar(c, x + 40, rowY, w - 110, 12, mapper.input[i], input);
        snprintf(line, sizeof line, "%+5d", (int)device.axis[i]);
        c.text(x + w - 60, rowY + 2, line, dim);
    }
    int boxY = y + 156;
    drawCrosshairBox(c, x + 10, boxY, 120, mapper.input[INPUT_RY], mapper.input[INPUT_RX], input, "TILT  RY / RX");
    drawCrosshairBox(c, x + w - 130, boxY, 120, mapper.input[INPUT_TX], mapper.input[INPUT_TY], input,
                     "SHIFT  TX / TY");

    c.text(x + 10, boxY + 130, "DEVICE BUTTONS", dim);
    drawButtonGrid(c, x + 10, boxY + 144, device.buttons, input);

    int infoY = boxY + 182;
    if (device.connected)
        snprintf(line, sizeof line, "%s  (%04X:%04X)", device.name, device.vid, device.pid);
    else
        snprintf(line, sizeof line, "No device");
    c.textClipped(x + 10, infoY, w - 20, line, device.connected ? text : dim);

    if (hooks.setDeviceCapture) drawCaptureSection(x + 10, infoY + 20, w - 20, y + h - 6);
}

void App::drawCaptureSection(int x, int y, int w, int bottom) {
    Canvas& c = canvas_;
    ui_.checkbox(widgetId(&profile.forceCapture), x, y, "Force capture", &profile.forceCapture);
    ui_.hint("Always grab the SpaceMouse, whatever app is in front");
    const char* status;
    std::string statusText;
    if (!captureError_.empty()) {
        status = captureError_.c_str();
    } else if (!captureApplied_) {
        status = "Not capturing";
    } else if (profile.forceCapture) {
        status = "Captured";
    } else {
        statusText = "Captured: " + runningCaptureApp_;
        status = statusText.c_str();
    }
    uint32_t statusColor = !captureError_.empty() ? warn : (captureApplied_ ? good : dim);
    int statusW = c.textWidth(status);
    c.textClipped(x + w - (statusW < 150 ? statusW : 150), y + 3, 150, status, statusColor);

    y += 20;
    ui_.checkbox(widgetId(&profile.captureWhenRunning), x, y,
                 "Capture SpaceMouse when these apps are running:", &profile.captureWhenRunning);
    y += 20;

    // Entry row: text field with process suggestions, and a file browser button.
    const int browseW = 26, fieldW = w - browseW - 4;
    uintptr_t fieldId = widgetId("captureApps.entry");
    bool submitted = ui_.textField(fieldId, x, y, fieldW, 18, appEntryText_, "Add app: type a process name");
    if (ui_.button(widgetId("captureApps.browse"), x + fieldW + 4, y, browseW, 18, "...")) browseRequested_ = true;
    ui_.hint("Pick an executable");

    bool fieldFocused = ui_.hasTextFocus();
    if (fieldFocused && hooks.listProcesses) {
        refreshSuggestions();
        if (ui_.input.key == VK_CODE_DOWN && !suggestions_.empty())
            suggestionHighlight_ = std::min(suggestionHighlight_ + 1, (int)suggestions_.size() - 1);
        if (ui_.input.key == VK_CODE_UP) suggestionHighlight_ = std::max(suggestionHighlight_ - 1, -1);
    } else {
        suggestions_.clear();
    }
    int picked =
        ui_.suggestionList(widgetId("captureApps.suggestions"), x, y + 19, fieldW, suggestions_, suggestionHighlight_);
    if (picked >= 0 && picked < (int)suggestions_.size()) {
        addCaptureApp(captureAppFromExecutable(suggestions_[picked]));
        appEntryText_.clear();
    } else if (submitted && !appEntryText_.empty()) {
        if (suggestionHighlight_ >= 0 && suggestionHighlight_ < (int)suggestions_.size())
            addCaptureApp(captureAppFromExecutable(suggestions_[suggestionHighlight_]));
        else
            addCaptureApp({appEntryText_, toLower(appEntryText_)});
        appEntryText_.clear();
    }
    y += 24;

    // App list with remove buttons; scrolls with the wheel.
    const int rowH = 16;
    int visibleRows = (bottom - y) / rowH;
    int count = (int)captureApps.size();
    if (ui_.hovered(x, y, w, visibleRows * rowH) && ui_.input.wheel) appListScroll_ -= ui_.input.wheel;
    appListScroll_ = std::max(0, std::min(appListScroll_, count - visibleRows));
    int removeIndex = -1;
    for (int row = 0; row < visibleRows && appListScroll_ + row < count; row++) {
        int i = appListScroll_ + row;
        const CaptureApp& app = captureApps[i];
        int rowY = y + row * rowH;
        bool running = app.name == runningCaptureApp_;
        uint32_t nameColor = !profile.captureWhenRunning ? faint : (running ? good : text);
        int nameEnd = c.textClipped(x + 2, rowY + 3, w - 110, app.name.c_str(), nameColor);
        c.textClipped(nameEnd + 8, rowY + 3, x + w - 24 - nameEnd - 8, app.match.c_str(), faint);
        if (ui_.button(widgetId("captureApps.remove", i), x + w - 18, rowY, 18, 14, "x")) removeIndex = i;
    }
    if (count > visibleRows) {
        int trackH = visibleRows * rowH;
        int knobH = std::max(8, trackH * visibleRows / count);
        int knobY = y + (trackH - knobH) * appListScroll_ / std::max(1, count - visibleRows);
        c.fillRect(x + w - 22, knobY, 2, knobH, dim);
    }
    if (removeIndex >= 0) {
        captureApps.erase(captureApps.begin() + removeIndex);
        if (captureAppWatchActive())
            pollProcesses();
        else
            runningCaptureApp_.clear();
    }
}

void App::drawOutputPanel(int x, int y, int w, int h) {
    Canvas& c = canvas_;
    ui_.panel(x, y, w, h, "OUTPUT - VIRTUAL HOTAS", output);
    char value[32];
    int rowY = y + 26;
    for (int o = 0; o < OUTPUT_AXIS_COUNT; o++, rowY += 18) {
        const AxisConfig& a = profile.axes[o];
        c.text(x + 10, rowY + 2, outputAxisName(o), a.enabled ? text : faint);
        if (a.enabled && a.mode == MODE_TRIM) c.text(x + 34, rowY + 2, "t", warn);
        drawBar(c, x + 44, rowY, w - 114, 12, mapper.output[o], output, a.enabled && profile.outputEnabled);
        snprintf(value, sizeof value, "%+.3f", mapper.output[o]);
        c.text(x + w - 60, rowY + 2, value, a.enabled ? dim : faint);
    }
    int boxY = y + 192;
    drawCrosshairBox(c, x + 10, boxY, 140, mapper.output[OUTPUT_X], mapper.output[OUTPUT_Y], output, "STICK  X / Y");
    drawCrosshairBox(c, x + 158, boxY, 140, mapper.output[OUTPUT_RZ], -mapper.output[OUTPUT_Z], output,
                     "RUDDER RZ / COLL Z");

    c.text(x + 10, boxY + 152, "VIRTUAL BUTTONS", dim);
    drawButtonGrid(c, x + 10, boxY + 166, profile.outputEnabled ? mapper.joystickButtons : 0, output);

    int hatY = boxY + 214;
    c.text(x + 10, hatY + 14, "HAT", dim);
    static const int kHatX[8] = {0, 1, 1, 1, 0, -1, -1, -1}, kHatY[8] = {-1, -1, 0, 1, 1, 1, 0, -1};
    int hatCenterX = x + 60, hatCenterY = hatY + 17;
    for (int row = -1; row <= 1; row++)
        for (int col = -1; col <= 1; col++) {
            bool lit = mapper.hat >= 0 && profile.outputEnabled && kHatX[mapper.hat] == col && kHatY[mapper.hat] == row;
            int cellX = hatCenterX + col * 12 - 5, cellY = hatCenterY + row * 12 - 5;
            c.fillRect(cellX, cellY, 11, 11, lit ? output : field);
            c.strokeRect(cellX, cellY, 11, 11, lit ? output : border);
        }
    bool sending = profile.outputEnabled && pad && pad->isOpen();
    const char* state = !profile.outputEnabled ? "Output paused (centered)"
                                               : (sending ? "Sending to virtual device" : "No virtual device");
    c.text(x + 100, hatY + 14, state, sending ? good : warn);
    drawWrapped(c, x + 10, hatY + 46, w - 20, "t = trim axis (sticky, holds its value)", faint);

    if (pad && pad->needsConfiguration()) {  // works partly until configured: few buttons, no hat
        int boxY = hatY + 70, boxH = 72;
        c.fillRect(x + 10, boxY, w - 20, boxH, blendColor(panel, warn, 40));
        c.strokeRect(x + 10, boxY, w - 20, boxH, warn);
        c.text(x + 18, boxY + 8, "Virtual device not configured", warn, true);
        drawWrapped(c, x + 18, boxY + 22, w - 36, "Buttons past 8 and the hat will not work until it is.", text);
        const char* label = pad->configureLabel() ? pad->configureLabel() : "Configure";
        if (ui_.button(widgetId("output.configure"), x + 18, boxY + 44, w - 36, 20, label, true, warn))
            padConfigureRequested_ = true;
    }
}

void App::drawEditor(int x, int y, int w, int h) {
    static const char* const kTabs[4] = {"AXES", "BUTTONS", "INPUTS", "SETUP"};
    for (int i = 0; i < 4; i++)
        if (ui_.button(widgetId("tab", i), x + i * 78, y, 76, 20, kTabs[i], tab_ == i, output)) {
            tab_ = i;
            capturingButton_ = -1;
        }
    ui_.panel(x, y + 24, w, h - 24, nullptr, output);
    int contentX = x + 8, contentY = y + 32, contentW = w - 16;
    switch (tab_) {
    case 0: drawAxesTab(contentX, contentY, contentW); break;
    case 1: drawButtonsTab(contentX, contentY, contentW); break;
    case 2: drawInputsTab(contentX, contentY, contentW); break;
    default: drawSetupTab(contentX, contentY, contentW, h - 40); break;
    }
}

void App::drawAxesTab(int x, int y, int w) {
    Canvas& c = canvas_;
    AxisConfig& a = profile.axes[selectedAxis_];
    const int group = a.group;
    const uint32_t color = groupColor(group);

    // Axis selector. Group members share the group color; every grouped axis gets a color strip.
    for (int o = 0; o < OUTPUT_AXIS_COUNT; o++) {
        int axisGroup = profile.axes[o].group;
        bool sibling = group > 0 && o != selectedAxis_ && axisGroup == group;
        uint32_t onColor = sibling ? color : (profile.axes[o].enabled ? output : dim);
        int bx = x + o * 37;
        if (ui_.button(widgetId("axis", o), bx, y, 35, 18, outputAxisName(o), o == selectedAxis_ || sibling, onColor))
            selectedAxis_ = o;
        if (axisGroup > 0) c.fillRect(bx + 2, y + 16, 31, 2, groupColor(axisGroup));
    }

    const bool trimMode = a.mode == MODE_TRIM;
    const int labelW = 90, controlX = x + labelW, controlW = w - labelW, rowH = 19, controlH = 15;
    int rowY = y + 25;
    char text[64], groupTip[64];
    snprintf(groupTip, sizeof groupTip, "%s: edits apply to every axis in this group", axisGroupName(group));

    auto label = [&](const char* s, bool enabled) { c.text(x, rowY + 4, s, enabled ? theme::text : faint); };
    // Settings shared within a group are tinted with the group color and explain why on hover.
    auto groupedBegin = [&](bool enabled) {
        ui_.accent = group > 0 ? color : theme::input;
        ui_.disabled = !enabled;
    };
    auto groupedEnd = [&]() {
        if (group > 0) ui_.hint(groupTip, color);
        ui_.accent = theme::input;
        ui_.disabled = false;
    };

    ui_.checkbox(widgetId(&a.enabled), x, rowY, "Enabled", &a.enabled);
    ui_.checkbox(widgetId(&a.invert), x + 110, rowY, "Invert output", &a.invert);
    rowY += rowH + 2;

    label("Group", true);
    ui_.accent = group > 0 ? color : theme::input;
    ui_.dropdown(widgetId(&a.group), controlX, rowY, controlW, controlH, &a.group, 0, kAxisGroups + 1, groupItem,
                 nullptr);
    ui_.hint("Axes in the same group are adjusted together", group > 0 ? color : dim);
    ui_.accent = theme::input;
    rowY += rowH + 2;

    label("Mode", true);
    groupedBegin(true);
    ui_.dropdown(widgetId(&a.mode), controlX, rowY, controlW, controlH, &a.mode, 0, MODE_COUNT, modeItem, nullptr);
    groupedEnd();
    rowY += rowH;

    label("Trim rate", trimMode);
    groupedBegin(trimMode);
    ui_.slider(widgetId(&a.trimRate), controlX, rowY, controlW, controlH, &a.trimRate, 0.05f, 5.0f, 0.5f, "%.2f /s");
    groupedEnd();
    rowY += rowH;

    label("Sticky zero", trimMode);
    groupedBegin(trimMode);
    ui_.slider(widgetId(&a.stickyZero), controlX, rowY, controlW, controlH, &a.stickyZero, 0.0f, 1.0f, 0.3f, "%.2f");
    groupedEnd();
    if (trimMode && group == 0) ui_.hint("Extra push needed to leave zero after crossing it");
    rowY += rowH;

    for (int k = 0; k < kMixSources; k++, rowY += rowH) {
        snprintf(text, sizeof text, "Source %d", k + 1);
        label(text, true);
        MixSource& m = a.mix[k];
        ui_.dropdown(widgetId(&m.input), controlX, rowY, 70, controlH, &m.input, -1, INPUT_AXIS_COUNT + 1, inputItem,
                     nullptr);
        if (m.input >= 0)
            ui_.slider(widgetId(&m.weight), controlX + 74, rowY, controlW - 74, controlH, &m.weight, -2.0f, 2.0f, 1.0f,
                       "x %+.2f");
    }

    label("Deadzone", true);
    groupedBegin(true);
    ui_.slider(widgetId(&a.deadzone), controlX, rowY, controlW, controlH, &a.deadzone, 0.0f, 0.5f, 0.05f, "%.3f");
    groupedEnd();
    rowY += rowH;
    label("DZ shape", true);
    groupedBegin(true);
    ui_.dropdown(widgetId(&a.deadzoneShape), controlX, rowY, controlW, controlH, &a.deadzoneShape, 0,
                 DEADZONE_SHAPE_COUNT, deadzoneItem, nullptr);
    groupedEnd();
    rowY += rowH;
    bool smoothDeadzone = a.deadzoneShape == DEADZONE_SMOOTH;
    label("DZ softness", smoothDeadzone);
    groupedBegin(smoothDeadzone);
    ui_.slider(widgetId(&a.deadzoneSoftness), controlX, rowY, controlW, controlH, &a.deadzoneSoftness, 0.0f, 1.0f, 0.3f,
               "%.2f");
    groupedEnd();
    rowY += rowH;
    label("Saturation", true);
    groupedBegin(true);
    ui_.slider(widgetId(&a.saturation), controlX, rowY, controlW, controlH, &a.saturation, 0.2f, 1.0f, 0.85f, "%.2f");
    groupedEnd();
    rowY += rowH;
    label("Curve", true);
    groupedBegin(true);
    ui_.dropdown(widgetId(&a.curve), controlX, rowY, controlW, controlH, &a.curve, 0, CURVE_COUNT, curveItem, nullptr);
    groupedEnd();
    rowY += rowH;
    bool curved = a.curve != CURVE_LINEAR;
    label("Steepness", curved);
    groupedBegin(curved);
    ui_.slider(widgetId(&a.steepness), controlX, rowY, controlW, controlH, &a.steepness, -1.0f, 1.0f, 0.25f, "%+.2f");
    groupedEnd();
    rowY += rowH;
    label("Gain", true);
    groupedBegin(true);
    ui_.slider(widgetId(&a.gain), controlX, rowY, controlW, controlH, &a.gain, 0.0f, 2.0f, 1.0f, "%.2f");
    groupedEnd();
    rowY += rowH;
    label("Smoothing", true);
    groupedBegin(true);
    ui_.slider(widgetId(&a.smoothing), controlX, rowY, controlW, controlH, &a.smoothing, 0.0f, 0.5f, 0.02f, "%.3f s");
    groupedEnd();
    rowY += rowH + 6;

    // Response plot: curve, identity diagonal, deadzone band and the live input point.
    const int size = 136;
    int plotX = x, plotY = rowY, half = size / 2;
    c.fillRect(plotX, plotY, size, size, field);
    c.strokeRect(plotX, plotY, size, size, border);
    int deadzoneW = (int)lroundf(a.deadzone * a.saturation * half);
    if (deadzoneW > 0)
        c.fillRect(plotX + half - deadzoneW, plotY + 1, 2 * deadzoneW + 1, size - 2, blendColor(field, bad, 40));
    for (int i = 1; i < 4; i++) {
        uint32_t gridColor = i == 2 ? border : blendColor(field, border, 110);
        c.fillRect(plotX + i * size / 4, plotY + 1, 1, size - 2, gridColor);
        c.fillRect(plotX + 1, plotY + i * size / 4, size - 2, 1, gridColor);
    }
    c.line(plotX + 1, plotY + size - 2, plotX + size - 2, plotY + 1, blendColor(field, border, 200));
    int prevX = 0, prevY = 0;
    for (int i = 0; i < size - 2; i++) {
        float in = -1.0f + 2.0f * i / (size - 3);
        float out = applyResponse(a, in) * (a.invert ? -1.0f : 1.0f);
        int px = plotX + 1 + i, py = plotY + half - (int)lroundf(out * (half - 2));
        if (i) c.line(prevX, prevY, px, py, output);
        prevX = px;
        prevY = py;
    }
    int dotX = plotX + half + (int)lroundf(mapper.mixed[selectedAxis_] * (half - 2));
    int dotY = plotY + half - (int)lroundf(mapper.shaped[selectedAxis_] * (half - 2));
    c.fillRect(dotX, plotY + 1, 1, size - 2, blendColor(field, input, 120));
    c.disc(dotX, dotY, 3, input);
    c.text(plotX + 4, plotY + 4, "response", dim);

    int infoX = x + size + 10, infoY = plotY + 2;
    snprintf(text, sizeof text, "input %+.3f", mapper.mixed[selectedAxis_]);
    c.text(infoX, infoY, text, input);
    snprintf(text, sizeof text, "resp  %+.3f", mapper.shaped[selectedAxis_]);
    c.text(infoX, infoY + 13, text, theme::text);
    snprintf(text, sizeof text, "out   %+.3f", mapper.output[selectedAxis_]);
    c.text(infoX, infoY + 26, text, output);
    snprintf(text, sizeof text, "trim  %+.3f", mapper.trim[selectedAxis_]);
    c.text(infoX, infoY + 39, text, trimMode ? warn : faint);
    ui_.disabled = !trimMode;
    if (ui_.button(widgetId("axis.zeroTrim"), infoX, infoY + 58, w - size - 10, 18, "Zero trim"))
        mapper.zeroTrim(selectedAxis_);
    ui_.disabled = false;
    drawWrapped(c, infoX, infoY + 84, w - size - 10, "Drag or wheel: adjust. Right-click: reset.", faint);
}

void App::drawButtonsTab(int x, int y, int w) {
    Canvas& c = canvas_;
    const int arrowW = 24, boxX = x + 86, boxW = w - 86 - arrowW - 2;
    char label[48];
    for (int b = 0; b < kDeviceButtons; b++) {
        int rowY = y + b * 16;
        bool capturing = capturingButton_ == b;
        bool rowHovered = ui_.hovered(x - 4, rowY - 1, w + 8, 16);
        if ((device.buttons >> b) & 1u) c.fillRect(x - 4, rowY - 1, w + 8, 16, blendColor(panel, input, 70));
        const char* buttonName = spaceMouseButtonName(device.pid, b);
        snprintf(label, sizeof label, "B%d  %s", b, buttonName ? buttonName : "");
        c.textClipped(x, rowY + 3, 82, label, buttonName ? text : dim);

        const char* mapped = capturing ? "Press key / mouse button..." : buttonTarget(buttonTargets_[b]).name;
        if (ui_.button(widgetId("button.capture", b), boxX, rowY, boxW, 14, mapped, capturing, warn))
            capturingButton_ = capturing ? -1 : b;
        ui_.hint(capturing ? "Esc: cancel   Del: clear" : "Click, then press a key or mouse button. Del: clear");

        DropdownStyle style;
        style.arrowOnly = true;
        style.listWidth = 180;
        style.color = output;
        ui_.dropdown(widgetId(&buttonTargets_[b]), boxX + boxW + 2, rowY, arrowW, 14, &buttonTargets_[b], 0,
                     buttonTargetCount(), targetItem, nullptr, style);
        ui_.hint("All targets");

        if (!capturing && rowHovered && !ui_.hasTextFocus() && ui_.input.key == VK_CODE_DELETE) buttonTargets_[b] = 0;
    }
}

void App::drawInputsTab(int x, int y, int w) {
    Canvas& c = canvas_;
    char value[16];
    int rowY = y + 30;
    for (int i = 0; i < INPUT_AXIS_COUNT; i++, rowY += 24) {
        InputConfig& in = profile.inputs[i];
        c.text(x, rowY + 4, inputAxisName(i), text);
        ui_.checkbox(widgetId(&in.invert), x + 28, rowY + 1, "Inv", &in.invert);
        ui_.slider(widgetId(&in.range), x + 84, rowY, 150, 16, &in.range, 50.0f, 1000.0f, 350.0f, "%.0f");
        ui_.hint("Raw counts at full deflection");
        snprintf(value, sizeof value, "%+5d", (int)device.axis[i]);
        c.text(x + 242, rowY + 4, value, dim);
    }
    drawWrapped(c, x, rowY + 10, w,
                "Raw directions:\n"
                "TX+  slide cap right\n"
                "TY+  slide cap toward you\n"
                "TZ+  push cap down\n"
                "RX+  tilt top toward you\n"
                "RY+  tilt top to the left\n"
                "RZ+  twist clockwise",
                text);
}

void App::drawSetupTab(int x, int y, int w, int h) {
    Canvas& c = canvas_;
    const int labelW = 120, controlH = 16;
    int rowY = y;

    c.text(x, rowY + 4, "Virtual device #", text);
    ui_.dropdown(widgetId(&profile.vjoyDevice), x + labelW, rowY, w - labelW, controlH, &profile.vjoyDevice, 1, 16,
                 numberItem, nullptr);
    rowY += 22;
    c.text(x, rowY + 4, "UI scale", text);
    ui_.dropdown(widgetId(&profile.uiScale), x + labelW, rowY, w - labelW, controlH, &profile.uiScale, 0, 4, scaleItem,
                 nullptr);
    rowY += 22;
    ui_.checkbox(widgetId(&profile.openWindowAtStart), x, rowY, "Open this window at start",
                 &profile.openWindowAtStart);
    rowY += 20;
    if (hooks.setStartWithSystem && hooks.startWithSystemLabel) {
        ui_.checkbox(widgetId(&profile.startWithSystem), x, rowY, hooks.startWithSystemLabel, &profile.startWithSystem);
        rowY += 20;
    }
    rowY += 6;

    c.text(x, rowY, "Output backend:", dim);
    rowY = drawWrapped(c, x, rowY + 13, w, pad ? pad->status() : "none", pad && pad->isOpen() ? good : warn) + 6;
    const char* configureLabel = pad ? pad->configureLabel() : nullptr;
    int buttonW = configureLabel ? (w - 6) / 2 : w;
    if (configureLabel && ui_.button(widgetId("setup.configure"), x, rowY, buttonW, 20, configureLabel))
        padConfigureRequested_ = true;
    if (ui_.button(widgetId("setup.reconnect"), configureLabel ? x + buttonW + 6 : x, rowY, buttonW, 20, "Reconnect"))
        reconnectPad();
    rowY += 28;
    if (pad) rowY = drawWrapped(c, x, rowY, w, pad->help(), dim);
    rowY += 8;

    c.text(x, rowY, "Config file:", dim);
    drawWrapped(c, x, rowY + 13, w, configPath_.c_str(), faint);

    if (ui_.button(widgetId("setup.quit"), x, y + h - 24, w, 20, "Quit SpacemouseHOTAS")) quitRequested = true;
}
