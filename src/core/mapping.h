// Settings model and the mapping engine: 3D mouse axes/buttons in, virtual HOTAS state out.
#pragma once
#include <cstdint>

enum InputAxis { INPUT_TX, INPUT_TY, INPUT_TZ, INPUT_RX, INPUT_RY, INPUT_RZ, INPUT_AXIS_COUNT };
enum OutputAxis {
    OUTPUT_X,
    OUTPUT_Y,
    OUTPUT_Z,
    OUTPUT_RX,
    OUTPUT_RY,
    OUTPUT_RZ,
    OUTPUT_SL0,
    OUTPUT_SL1,
    OUTPUT_AXIS_COUNT
};

constexpr int kMixSources = 3;
constexpr int kDeviceButtons = 32;
constexpr int kJoystickButtons = 32;
constexpr int kAxisGroups = 5;  // group 0 = not grouped

enum AxisMode { MODE_DIRECT, MODE_TRIM, MODE_COUNT };
enum DeadzoneShape { DEADZONE_HARD, DEADZONE_SCALED, DEADZONE_SMOOTH, DEADZONE_SHAPE_COUNT };
enum CurveType { CURVE_LINEAR, CURVE_POWER, CURVE_EXPO, CURVE_SCURVE, CURVE_COUNT };
enum HatDirection { HAT_UP, HAT_RIGHT, HAT_DOWN, HAT_LEFT, HAT_DIRECTION_COUNT };
enum ButtonAction {
    ACTION_NONE,
    ACTION_JOY_BUTTON,  // param: joystick button 0..31
    ACTION_HAT,         // param: HatDirection
    ACTION_TRIM_UP,     // param: OutputAxis
    ACTION_TRIM_DOWN,   // param: OutputAxis
    ACTION_TRIM_ZERO,   // param: OutputAxis
    ACTION_TOGGLE_OUTPUT,
    ACTION_KEY,    // param: Windows virtual-key code
    ACTION_MOUSE,  // param: MouseButton
    ACTION_COUNT
};

struct MixSource {
    int input;     // InputAxis, -1 = unused
    float weight;  // -2..2
};

struct InputConfig {
    float range;  // raw counts at full deflection
    bool invert;
};

struct AxisConfig {
    bool enabled;
    int mode;          // AxisMode
    float trimRate;    // trim mode: full scale per second at full deflection
    float stickyZero;  // trim mode: 0..1, extra push needed to leave zero after crossing it
    MixSource mix[kMixSources];
    float deadzone;          // 0..0.5 of deflection
    int deadzoneShape;       // DeadzoneShape
    float deadzoneSoftness;  // 0..1, width of the smooth transition
    float saturation;        // deflection that already gives full output, 0.2..1
    int curve;               // CurveType
    float steepness;         // -1..1
    float gain;              // 0..2
    bool invert;
    float smoothing;  // low-pass time constant in seconds, 0 = off
    int group;        // 0 = none, 1..kAxisGroups; grouped axes are edited together
};

struct ButtonMapping {
    int action;  // ButtonAction
    int param;
};

// Plain data on purpose: the UI detects edits with memcmp.
struct Profile {
    InputConfig inputs[INPUT_AXIS_COUNT];
    AxisConfig axes[OUTPUT_AXIS_COUNT];
    ButtonMapping buttons[kDeviceButtons];
    bool outputEnabled;
    int vjoyDevice;  // 1..16
    int uiScale;     // 0 = follow display DPI
    bool openWindowAtStart;
    bool startWithSystem;
    bool forceCapture;        // always grab the 3D mouse through the vendor driver
    bool captureWhenRunning;  // grab while a listed capture app is running
};

void setDefaultProfile(Profile& profile);
void setDefaultAxis(AxisConfig& axis);
// Applies every grouped setting that differs between `before` and `after` to `target`.
void copyGroupedSettings(const AxisConfig& before, const AxisConfig& after, AxisConfig& target);

const char* inputAxisName(int axis);   // "TX".."RZ", "-" for none
const char* outputAxisName(int axis);  // "X".."SL1"
const char* axisModeName(int mode);
const char* deadzoneShapeName(int shape);
const char* curveName(int curve);
const char* hatDirectionName(int direction);
const char* axisGroupName(int group);  // "Off", "Group 1".."Group 5"

// Flat list of every button mapping choice, for menus.
struct ButtonTarget {
    int action;
    int param;
    char name[24];
};
int buttonTargetCount();
const ButtonTarget& buttonTarget(int index);
int findButtonTarget(const ButtonMapping& mapping);  // index, 0 (= none) if unknown

float applyCurve(int curve, float steepness, float deflection);  // 0..1 -> 0..1
float applyResponse(const AxisConfig& axis, float value);        // saturation, deadzone, curve, gain; -1..1

struct Mapper {
    float rawInput[INPUT_AXIS_COUNT] = {};
    float input[INPUT_AXIS_COUNT] = {};  // normalized -1..1
    uint32_t deviceButtons = 0;

    float mixed[OUTPUT_AXIS_COUNT] = {};   // after mixing, before response
    float shaped[OUTPUT_AXIS_COUNT] = {};  // after response and invert
    float trim[OUTPUT_AXIS_COUNT] = {};    // trim mode value
    float target[OUTPUT_AXIS_COUNT] = {};
    float output[OUTPUT_AXIS_COUNT] = {};  // final value, -1..1

    uint32_t joystickButtons = 0;
    int hat = -1;           // -1 centered, 0..7 = N, NE, E, SE, S, SW, W, NW
    uint32_t keys[8] = {};  // 256-bit set of held key codes
    uint32_t mouseButtons = 0;
    bool toggleOutputRequested = false;

    // Returns true while the outputs keep changing without new input (trim moving, smoothing settling).
    bool update(const Profile& profile, float dt, bool outputActive);
    void zeroTrim(int axis);

private:
    uint32_t previousButtons_ = 0;
    bool trimAtZero_[OUTPUT_AXIS_COUNT] = {true, true, true, true, true, true, true, true};
    float zeroPush_[OUTPUT_AXIS_COUNT] = {};
    void integrateTrim(int axis, float delta, float stickyZero);
};
