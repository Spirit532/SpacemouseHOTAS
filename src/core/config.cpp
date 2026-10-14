#include "core/config.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {

FILE* openUtf8Path(const char* path, const char* mode) {
#ifdef _WIN32
    wchar_t widePath[1024], wideMode[8];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, widePath, 1024)) return nullptr;
    MultiByteToWideChar(CP_UTF8, 0, mode, -1, wideMode, 8);
    return _wfopen(widePath, wideMode);
#else
    return fopen(path, mode);
#endif
}

enum FieldType { FIELD_FLOAT, FIELD_INT, FIELD_BOOL };

// Visits every persisted Profile field; shared by load and save so the key list exists once.
template <typename Visitor>
void forEachField(Profile& p, Visitor visit) {
    char key[64];
    visit("enabled", FIELD_BOOL, &p.outputEnabled);
    visit("device_id", FIELD_INT, &p.vjoyDevice);
    visit("ui_scale", FIELD_INT, &p.uiScale);
    visit("show_on_start", FIELD_BOOL, &p.openWindowAtStart);
    visit("start_with_system", FIELD_BOOL, &p.startWithSystem);
    visit("force_capture", FIELD_BOOL, &p.forceCapture);
    visit("capture_when_running", FIELD_BOOL, &p.captureWhenRunning);
    for (int i = 0; i < INPUT_AXIS_COUNT; i++) {
        const char* n = inputAxisName(i);
        snprintf(key, sizeof key, "in.%s.range", n);
        visit(key, FIELD_FLOAT, &p.inputs[i].range);
        snprintf(key, sizeof key, "in.%s.invert", n);
        visit(key, FIELD_BOOL, &p.inputs[i].invert);
    }
    for (int o = 0; o < OUTPUT_AXIS_COUNT; o++) {
        AxisConfig& a = p.axes[o];
        const char* n = outputAxisName(o);
        auto field = [&](const char* suffix, FieldType type, void* value) {
            snprintf(key, sizeof key, "axis.%s.%s", n, suffix);
            visit(key, type, value);
        };
        field("enabled", FIELD_BOOL, &a.enabled);
        field("mode", FIELD_INT, &a.mode);
        field("rate", FIELD_FLOAT, &a.trimRate);
        field("sticky_zero", FIELD_FLOAT, &a.stickyZero);
        for (int k = 0; k < kMixSources; k++) {
            char suffix[16];
            snprintf(suffix, sizeof suffix, "mix%d.src", k);
            field(suffix, FIELD_INT, &a.mix[k].input);
            snprintf(suffix, sizeof suffix, "mix%d.w", k);
            field(suffix, FIELD_FLOAT, &a.mix[k].weight);
        }
        field("deadzone", FIELD_FLOAT, &a.deadzone);
        field("dz_shape", FIELD_INT, &a.deadzoneShape);
        field("dz_soft", FIELD_FLOAT, &a.deadzoneSoftness);
        field("saturation", FIELD_FLOAT, &a.saturation);
        field("curve", FIELD_INT, &a.curve);
        field("steepness", FIELD_FLOAT, &a.steepness);
        field("gain", FIELD_FLOAT, &a.gain);
        field("invert", FIELD_BOOL, &a.invert);
        field("smoothing", FIELD_FLOAT, &a.smoothing);
        field("group", FIELD_INT, &a.group);
    }
    for (int b = 0; b < kDeviceButtons; b++) {
        snprintf(key, sizeof key, "btn.%d.action", b);
        visit(key, FIELD_INT, &p.buttons[b].action);
        snprintf(key, sizeof key, "btn.%d.param", b);
        visit(key, FIELD_INT, &p.buttons[b].param);
    }
}

template <typename T>
void clampValue(T& v, T lo, T hi) {
    if (v < lo) v = lo;
    if (v > hi) v = hi;
}

void clampProfile(Profile& p) {
    clampValue(p.vjoyDevice, 1, 16);
    clampValue(p.uiScale, 0, 3);
    for (InputConfig& in : p.inputs) clampValue(in.range, 10.0f, 5000.0f);
    for (AxisConfig& a : p.axes) {
        clampValue(a.mode, 0, MODE_COUNT - 1);
        clampValue(a.trimRate, 0.01f, 10.0f);
        clampValue(a.stickyZero, 0.0f, 1.0f);
        for (MixSource& m : a.mix) {
            clampValue(m.input, -1, INPUT_AXIS_COUNT - 1);
            clampValue(m.weight, -2.0f, 2.0f);
        }
        clampValue(a.deadzone, 0.0f, 0.5f);
        clampValue(a.deadzoneShape, 0, DEADZONE_SHAPE_COUNT - 1);
        clampValue(a.deadzoneSoftness, 0.0f, 1.0f);
        clampValue(a.saturation, 0.2f, 1.0f);
        clampValue(a.curve, 0, CURVE_COUNT - 1);
        clampValue(a.steepness, -1.0f, 1.0f);
        clampValue(a.gain, 0.0f, 2.0f);
        clampValue(a.smoothing, 0.0f, 0.5f);
        clampValue(a.group, 0, kAxisGroups);
    }
    for (ButtonMapping& b : p.buttons) {
        clampValue(b.action, 0, ACTION_COUNT - 1);
        clampValue(b.param, 0, 255);
    }
}

void trimLineEnd(char* s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
}

}  // namespace

bool loadConfig(const char* path, Profile& profile, std::vector<CaptureApp>& apps) {
    FILE* f = openUtf8Path(path, "rb");
    if (!f) return false;
    std::vector<CaptureApp> fileApps;
    bool fileHasAppList = false;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        trimLineEnd(line);
        char* eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        const char* key = line;
        const char* value = eq + 1;
        if (!strcmp(key, "capture_apps")) {
            fileHasAppList = true;
        } else if (!strcmp(key, "capture_app")) {
            const char* bar = strchr(value, '|');
            if (bar) fileApps.push_back({std::string(value, bar), bar + 1});
        } else {
            forEachField(profile, [&](const char* name, FieldType type, void* field) {
                if (strcmp(name, key) != 0) return;
                if (type == FIELD_FLOAT)
                    *(float*)field = (float)atof(value);
                else if (type == FIELD_INT)
                    *(int*)field = atoi(value);
                else
                    *(bool*)field = atoi(value) != 0;
            });
        }
    }
    fclose(f);
    clampProfile(profile);
    if (fileHasAppList) apps = fileApps;
    return true;
}

bool saveConfig(const char* path, const Profile& profile, const std::vector<CaptureApp>& apps) {
    FILE* f = openUtf8Path(path, "wb");
    if (!f) return false;
    Profile p = profile;
    fputs("# SpacemouseHOTAS configuration\n", f);
    forEachField(p, [&](const char* name, FieldType type, void* field) {
        if (type == FIELD_FLOAT)
            fprintf(f, "%s=%g\n", name, *(float*)field);
        else if (type == FIELD_INT)
            fprintf(f, "%s=%d\n", name, *(int*)field);
        else
            fprintf(f, "%s=%d\n", name, *(bool*)field ? 1 : 0);
    });
    // Entry format: name|match. The name may not contain '|'.
    fprintf(f, "capture_apps=%d\n", (int)apps.size());
    for (const CaptureApp& app : apps) fprintf(f, "capture_app=%s|%s\n", app.name.c_str(), app.match.c_str());
    bool ok = ferror(f) == 0;
    fclose(f);
    return ok;
}
