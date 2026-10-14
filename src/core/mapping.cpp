#include "core/mapping.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include "core/keys.h"

namespace {

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

template <int N>
const char* nameAt(const char* const (&names)[N], int index, const char* fallback) {
    return index >= 0 && index < N ? names[index] : fallback;
}

const char* const kInputNames[] = {"TX", "TY", "TZ", "RX", "RY", "RZ"};
const char* const kOutputNames[] = {"X", "Y", "Z", "RX", "RY", "RZ", "SL0", "SL1"};
const char* const kModeNames[] = {"Direct", "Trim (sticky)"};
const char* const kDeadzoneNames[] = {"Hard", "Scaled", "Smooth"};
const char* const kCurveNames[] = {"Linear", "Power", "Expo", "S-curve", "Exponential", "Custom"};
const char* const kMixModeNames[] = {"Additive", "Average"};
const char* const kHatNames[] = {"Up", "Right", "Down", "Left"};
const char* const kGroupNames[] = {"Off", "Group 1", "Group 2", "Group 3", "Group 4", "Group 5"};

}  // namespace

const char* inputAxisName(int axis) { return nameAt(kInputNames, axis, "-"); }
const char* outputAxisName(int axis) { return nameAt(kOutputNames, axis, "-"); }
const char* axisModeName(int mode) { return nameAt(kModeNames, mode, "?"); }
const char* deadzoneShapeName(int shape) { return nameAt(kDeadzoneNames, shape, "?"); }
const char* curveName(int curve) { return nameAt(kCurveNames, curve, "?"); }
const char* mixModeName(int mode) { return nameAt(kMixModeNames, mode, "?"); }
const char* hatDirectionName(int direction) { return nameAt(kHatNames, direction, "?"); }
const char* axisGroupName(int group) { return nameAt(kGroupNames, group, "?"); }

void setDefaultAxis(AxisConfig& a) {
    a.enabled = false;
    a.mode = MODE_DIRECT;
    a.trimRate = 0.5f;
    a.stickyZero = 0.3f;
    for (MixSource& m : a.mix) m = {-1, 1.0f};
    a.deadzone = 0.05f;
    a.deadzoneShape = DEADZONE_SMOOTH;
    a.deadzoneSoftness = 0.3f;
    a.saturation = 0.85f;
    a.curve = CURVE_EXPO;
    a.steepness = 0.25f;
    a.gain = 1.0f;
    a.invert = false;
    a.smoothing = 0.02f;
    a.group = 0;
    a.mixMode = MIX_ADD;
    for (int i = 0; i < kCustomCurvePoints; i++) a.customCurve[i] = (i + 1) / (float)kCustomCurvePoints;
}

// Feel tuned in Wardogs: full range, no deadzone, no smoothing.
static void setTunedAxis(AxisConfig& a, float steepness, float smoothing) {
    a.enabled = true;
    a.deadzone = 0.0f;
    a.deadzoneShape = DEADZONE_SCALED;
    a.deadzoneSoftness = 0.0f;
    a.saturation = 1.0f;
    a.curve = CURVE_EXPONENTIAL;
    a.steepness = steepness;
    a.smoothing = smoothing;
}

static void setStickAxis(AxisConfig& a, int tilt, int slide, float slideWeight) {
    setTunedAxis(a, 0.35f, 0.01f);
    a.mix[0] = {tilt, 1.0f};
    a.mix[1] = {slide, slideWeight};
    const float custom[kCustomCurvePoints] = {0.06f, 0.17f, 0.42f, 1.0f};
    memcpy(a.customCurve, custom, sizeof custom);
}

void setDefaultProfile(Profile& p) {
    memset(&p, 0, sizeof p);
    for (InputConfig& in : p.inputs) in = {350.0f, false};
    for (AxisConfig& a : p.axes) setDefaultAxis(a);

    // Stick: tilt for large inputs plus a small share of sliding the cap for fine corrections.
    setStickAxis(p.axes[OUTPUT_X], INPUT_RY, INPUT_TX, -0.25f);  // roll
    setStickAxis(p.axes[OUTPUT_Y], INPUT_RX, INPUT_TY, 0.25f);   // pitch, pull back = +Y

    AxisConfig& collective = p.axes[OUTPUT_Z];  // lift = up
    setTunedAxis(collective, 0.25f, 0.0f);
    collective.mix[0] = {INPUT_TZ, -1.0f};
    collective.deadzone = 0.005f;
    collective.deadzoneShape = DEADZONE_SMOOTH;
    collective.gain = 2.9f;
    collective.trimRate = 1.635f;

    AxisConfig& rudder = p.axes[OUTPUT_RZ];  // twist, clockwise = right
    rudder.enabled = true;
    rudder.mix[0] = {INPUT_RZ, 1.0f};
    rudder.deadzone = 0.1f;
    rudder.steepness = 0.3f;

    p.axes[OUTPUT_X].group = p.axes[OUTPUT_Y].group = 1;
    p.axes[OUTPUT_RX].group = p.axes[OUTPUT_RY].group = p.axes[OUTPUT_RZ].group = 2;
    p.axes[OUTPUT_SL0].group = p.axes[OUTPUT_SL1].group = 3;

    for (int b = 0; b < kDeviceButtons; b++) p.buttons[b] = {ACTION_JOY_BUTTON, b};

    p.outputEnabled = true;
    p.vjoyDevice = 1;
    p.uiScale = 0;
    p.openWindowAtStart = false;
    p.startWithSystem = true;
    p.forceCapture = false;
    p.captureWhenRunning = true;
}

void copyGroupedSettings(const AxisConfig& before, const AxisConfig& after, AxisConfig& target) {
#define COPY_IF_CHANGED(field) \
    if (after.field != before.field) target.field = after.field
    COPY_IF_CHANGED(mode);
    COPY_IF_CHANGED(trimRate);
    COPY_IF_CHANGED(stickyZero);
    COPY_IF_CHANGED(deadzone);
    COPY_IF_CHANGED(deadzoneShape);
    COPY_IF_CHANGED(deadzoneSoftness);
    COPY_IF_CHANGED(saturation);
    COPY_IF_CHANGED(curve);
    COPY_IF_CHANGED(steepness);
    COPY_IF_CHANGED(gain);
    COPY_IF_CHANGED(smoothing);
    COPY_IF_CHANGED(mixMode);
    for (int i = 0; i < kCustomCurvePoints; i++) COPY_IF_CHANGED(customCurve[i]);
#undef COPY_IF_CHANGED
}

// Button targets ------------------------------------------------------------------------------

namespace {

struct TargetList {
    ButtonTarget items[256];
    int count = 0;

    void add(int action, int param, const char* name) {
        if (count >= 256) return;
        items[count] = {action, param, {}};
        snprintf(items[count].name, sizeof items[count].name, "%s", name);
        count++;
    }

    TargetList() {
        char name[24];
        add(ACTION_NONE, 0, "-");
        for (int i = 0; i < kJoystickButtons; i++) {
            snprintf(name, sizeof name, "Joy %d", i + 1);
            add(ACTION_JOY_BUTTON, i, name);
        }
        for (int i = 0; i < HAT_DIRECTION_COUNT; i++) {
            snprintf(name, sizeof name, "Hat %s", hatDirectionName(i));
            add(ACTION_HAT, i, name);
        }
        for (int i = 0; i < OUTPUT_AXIS_COUNT; i++) {
            snprintf(name, sizeof name, "Trim %s +", outputAxisName(i));
            add(ACTION_TRIM_UP, i, name);
        }
        for (int i = 0; i < OUTPUT_AXIS_COUNT; i++) {
            snprintf(name, sizeof name, "Trim %s -", outputAxisName(i));
            add(ACTION_TRIM_DOWN, i, name);
        }
        for (int i = 0; i < OUTPUT_AXIS_COUNT; i++) {
            snprintf(name, sizeof name, "Trim %s zero", outputAxisName(i));
            add(ACTION_TRIM_ZERO, i, name);
        }
        add(ACTION_TOGGLE_OUTPUT, 0, "Toggle output");
        for (int i = 0; i < MOUSE_BUTTON_COUNT; i++) add(ACTION_MOUSE, i, mouseButtonName(i));
        for (int i = 0; i < kKeyCount; i++) {
            snprintf(name, sizeof name, "Key %s", kKeys[i].name);
            add(ACTION_KEY, kKeys[i].vk, name);
        }
    }
};

const TargetList& targetList() {
    static const TargetList list;
    return list;
}

}  // namespace

int buttonTargetCount() { return targetList().count; }

const ButtonTarget& buttonTarget(int index) {
    const TargetList& list = targetList();
    return list.items[index >= 0 && index < list.count ? index : 0];
}

int findButtonTarget(const ButtonMapping& mapping) {
    const TargetList& list = targetList();
    bool ignoresParam = mapping.action == ACTION_NONE || mapping.action == ACTION_TOGGLE_OUTPUT;
    for (int i = 0; i < list.count; i++)
        if (list.items[i].action == mapping.action && (ignoresParam || list.items[i].param == mapping.param)) return i;
    return 0;
}

// Response ------------------------------------------------------------------------------------

static float smoothstep(float a) { return a * a * (3 - 2 * a); }
static float inverseSmoothstep(float a) { return 0.5f - sinf(asinf(clampf(1 - 2 * a, -1.0f, 1.0f)) / 3.0f); }

// Catmull-Rom through (0,0) and the four custom points, mirrored past both ends so the slope there stays natural.
static float customCurve(const float* points, float a) {
    float y[kCustomCurvePoints + 3];
    y[1] = 0;
    for (int i = 0; i < kCustomCurvePoints; i++) y[i + 2] = points[i];
    y[0] = -y[2];  // odd symmetry around zero
    y[kCustomCurvePoints + 2] = 2 * y[kCustomCurvePoints + 1] - y[kCustomCurvePoints];
    float position = a * kCustomCurvePoints;
    int segment = position >= kCustomCurvePoints ? kCustomCurvePoints - 1 : (int)position;
    float t = position - segment;
    float p0 = y[segment], p1 = y[segment + 1], p2 = y[segment + 2], p3 = y[segment + 3];
    float value = 0.5f * (2 * p1 + (p2 - p0) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t +
                          (3 * p1 - p0 - 3 * p2 + p3) * t * t * t);
    return clampf(value, 0.0f, 1.0f);
}

// Steepness runs -2..2. Within -1..1 every curve keeps its original shape; beyond that each
// continues smoothly toward a stronger version of the same shape.
float applyCurve(const AxisConfig& c, float a) {
    a = clampf(a, 0.0f, 1.0f);
    float s = clampf(c.steepness, -kSteepnessLimit, kSteepnessLimit);
    switch (c.curve) {
    case CURVE_POWER: return powf(a, powf(4.0f, s));  // exponent 1/16..16
    case CURVE_EXPO: {
        float cube = a * a * a;
        if (s > 1) return (2 - s) * cube + (s - 1) * cube * a * a;  // toward a^5
        if (s >= 0) return (1 - s) * a + s * cube;                  // flatter center
        float lively = a + 0.5f * (a - cube);                       // s = -1 shape
        if (s >= -1) return a + (-s * 0.5f) * (a - cube);           // livelier center
        return (-1 - s) * cbrtf(a) + (2 + s) * lively;              // toward a^(1/3)
    }
    case CURVE_SCURVE: {
        if (s > 1) return (2 - s) * smoothstep(a) + (s - 1) * smoothstep(smoothstep(a));
        if (s >= 0) return a + (smoothstep(a) - a) * s;           // flat center and ends
        if (s >= -1) return a + (inverseSmoothstep(a) - a) * -s;  // steep center and ends
        return (2 + s) * inverseSmoothstep(a) + (-1 - s) * inverseSmoothstep(inverseSmoothstep(a));
    }
    case CURVE_EXPONENTIAL: {
        float k = s * 4.0f;  // + = slow start, fast finish; - = the reverse
        if (fabsf(k) < 0.001f) return a;
        return (expf(k * a) - 1) / (expf(k) - 1);
    }
    case CURVE_CUSTOM: return customCurve(c.customCurve, a);
    default: return a;
    }
}

float applyResponse(const AxisConfig& c, float value) {
    float sign = value < 0 ? -1.0f : 1.0f;
    float a = clampf(fabsf(value) / (c.saturation < 0.05f ? 0.05f : c.saturation), 0.0f, 1.0f);

    float deadzone = clampf(c.deadzone, 0.0f, 0.95f);
    if (a <= deadzone) {
        a = 0;
    } else if (c.deadzoneShape == DEADZONE_SCALED) {
        a = (a - deadzone) / (1 - deadzone);
    } else if (c.deadzoneShape == DEADZONE_SMOOTH) {
        // Quadratic ease-in that joins the linear part with matching slope.
        float t = (a - deadzone) / (1 - deadzone);
        float width = clampf(c.deadzoneSoftness, 0.001f, 1.0f);
        a = (t < width ? t * t / (2 * width) : t - width * 0.5f) / (1 - width * 0.5f);
    }  // DEADZONE_HARD keeps the raw value past the edge
    return clampf(sign * applyCurve(c, a) * c.gain, -1.0f, 1.0f);
}

// Mapper --------------------------------------------------------------------------------------

void Mapper::zeroTrim(int axis) {
    trim[axis] = 0;
    trimAtZero_[axis] = true;
    zeroPush_[axis] = 0;
}

// Trim integration with a detent at zero: crossing zero stops there, and leaving zero
// takes an extra push of stickyZero * 0.25 full scale in one direction.
void Mapper::integrateTrim(int axis, float delta, float stickyZero) {
    if (delta == 0) return;
    float detent = clampf(stickyZero, 0.0f, 1.0f) * 0.25f;
    if (detent <= 0) {
        trim[axis] = clampf(trim[axis] + delta, -1.0f, 1.0f);
        trimAtZero_[axis] = false;
        return;
    }
    if (trimAtZero_[axis] && trim[axis] == 0) {
        float& push = zeroPush_[axis];
        if (push != 0 && (push > 0) != (delta > 0)) push = 0;  // direction changed
        push += delta;
        if (fabsf(push) > detent) {
            trim[axis] = clampf(push - copysignf(detent, push), -1.0f, 1.0f);
            trimAtZero_[axis] = false;
            push = 0;
        }
        return;
    }
    float next = trim[axis] + delta;
    if (trim[axis] != 0 && (next == 0 || (trim[axis] > 0) != (next > 0))) {
        zeroTrim(axis);
        return;
    }
    trim[axis] = clampf(next, -1.0f, 1.0f);
}

bool Mapper::update(const Profile& p, float dt, bool outputActive) {
    for (int i = 0; i < INPUT_AXIS_COUNT; i++) {
        float range = p.inputs[i].range < 1.0f ? 1.0f : p.inputs[i].range;
        float v = clampf(rawInput[i] / range, -1.0f, 1.0f);
        input[i] = p.inputs[i].invert ? -v : v;
    }

    uint32_t pressedNow = deviceButtons & ~previousButtons_;
    previousButtons_ = deviceButtons;
    uint32_t joy = 0, mouse = 0;
    unsigned hatBits = 0;
    float trimPush[OUTPUT_AXIS_COUNT] = {};
    for (uint32_t& k : keys) k = 0;

    for (int b = 0; b < kDeviceButtons; b++) {
        const ButtonMapping& m = p.buttons[b];
        bool held = (deviceButtons >> b) & 1u;
        bool pressed = (pressedNow >> b) & 1u;
        bool axisParam = m.param >= 0 && m.param < OUTPUT_AXIS_COUNT;
        switch (m.action) {
        case ACTION_JOY_BUTTON:
            if (held && m.param >= 0 && m.param < kJoystickButtons) joy |= 1u << m.param;
            break;
        case ACTION_HAT:
            if (held && m.param >= 0 && m.param < HAT_DIRECTION_COUNT) hatBits |= 1u << m.param;
            break;
        case ACTION_TRIM_UP:
            if (held && axisParam) trimPush[m.param] += 1.0f;
            break;
        case ACTION_TRIM_DOWN:
            if (held && axisParam) trimPush[m.param] -= 1.0f;
            break;
        case ACTION_TRIM_ZERO:
            if (pressed && axisParam) zeroTrim(m.param);
            break;
        case ACTION_TOGGLE_OUTPUT:
            if (pressed) toggleOutputRequested = true;
            break;
        case ACTION_KEY:
            if (held && m.param > 0 && m.param < 256) keys[m.param >> 5] |= 1u << (m.param & 31);
            break;
        case ACTION_MOUSE:
            if (held && m.param >= 0 && m.param < MOUSE_BUTTON_COUNT) mouse |= 1u << m.param;
            break;
        }
    }
    joystickButtons = joy;
    mouseButtons = mouse;

    bool up = hatBits & (1u << HAT_UP), down = hatBits & (1u << HAT_DOWN);
    bool left = hatBits & (1u << HAT_LEFT), right = hatBits & (1u << HAT_RIGHT);
    if (up && down) up = down = false;
    if (left && right) left = right = false;
    static const int kHatFromRowCol[3][3] = {
        {7, 0, 1}, {6, -1, 2}, {5, 4, 3}};  // rows up/none/down, cols left/none/right
    hat = kHatFromRowCol[up ? 0 : (down ? 2 : 1)][left ? 0 : (right ? 2 : 1)];

    bool stillMoving = false;
    for (int o = 0; o < OUTPUT_AXIS_COUNT; o++) {
        const AxisConfig& c = p.axes[o];
        if (!c.enabled) {
            mixed[o] = shaped[o] = target[o] = output[o] = 0;
            continue;
        }
        // Additive sums the weighted sources (and clips). Average divides by the total weight, so the
        // weights only set each source's share.
        float sum = 0, totalWeight = 0;
        for (const MixSource& m : c.mix)
            if (m.input >= 0 && m.input < INPUT_AXIS_COUNT) {
                sum += m.weight * input[m.input];
                totalWeight += fabsf(m.weight);
            }
        if (c.mixMode == MIX_AVERAGE && totalWeight > 0.0001f) sum /= totalWeight;
        mixed[o] = clampf(sum, -1.0f, 1.0f);
        float y = applyResponse(c, mixed[o]);
        shaped[o] = c.invert ? -y : y;

        if (c.mode == MODE_TRIM) {
            if (outputActive) {
                integrateTrim(o, (shaped[o] + trimPush[o]) * c.trimRate * dt, c.stickyZero);
                if (shaped[o] != 0 || trimPush[o] != 0) stillMoving = true;
            }
            target[o] = trim[o];
        } else {
            target[o] = shaped[o];
        }

        if (c.smoothing <= 0.001f) {
            output[o] = target[o];
        } else if (dt <= 0) {
            if (output[o] != target[o]) stillMoving = true;  // settles on the next timed update
        } else {
            output[o] += (target[o] - output[o]) * (1.0f - expf(-dt / c.smoothing));
            if (fabsf(target[o] - output[o]) > 0.0005f)
                stillMoving = true;
            else
                output[o] = target[o];
        }
    }
    return stillMoving;
}
