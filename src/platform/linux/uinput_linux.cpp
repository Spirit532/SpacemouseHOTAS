#include "platform/linux/uinput_linux.h"
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "core/keys.h"

namespace {

const int kAxisCodes[kPadAxes] = {ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ, ABS_THROTTLE, ABS_RUDDER};
const int kMouseCodes[MOUSE_BUTTON_COUNT] = {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA};

int joystickButtonCode(int index) { return index < 16 ? BTN_TRIGGER + index : BTN_TRIGGER_HAPPY1 + (index - 16); }

int openUinput() {
    int fd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) fd = ::open("/dev/input/uinput", O_WRONLY | O_NONBLOCK);
    return fd;
}

bool createDevice(int fd, const char* name, uint16_t productId, uinput_user_dev& setup) {
    snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "%s", name);
    setup.id.bustype = BUS_USB;
    setup.id.vendor = 0x1209;
    setup.id.product = productId;
    setup.id.version = 1;
    return write(fd, &setup, sizeof setup) == (ssize_t)sizeof setup && ioctl(fd, UI_DEV_CREATE) >= 0;
}

bool emit(int fd, int type, int code, int value) {
    input_event event;
    memset(&event, 0, sizeof event);
    event.type = type;
    event.code = code;
    event.value = value;
    return write(fd, &event, sizeof event) == (ssize_t)sizeof event;
}

class UinputKeyboard {
public:
    ~UinputKeyboard() {
        if (fd_ >= 0) {
            ioctl(fd_, UI_DEV_DESTROY);
            ::close(fd_);
        }
    }

    void press(int code, bool down) {
        if (!ensureOpen()) return;
        emit(fd_, EV_KEY, code, down);
        emit(fd_, EV_SYN, SYN_REPORT, 0);
    }

private:
    int fd_ = -1;
    bool failed_ = false;

    bool ensureOpen() {
        if (fd_ >= 0) return true;
        if (failed_) return false;
        fd_ = openUinput();
        if (fd_ < 0) {
            failed_ = true;
            return false;
        }
        ioctl(fd_, UI_SET_EVBIT, EV_KEY);
        ioctl(fd_, UI_SET_EVBIT, EV_REL);
        ioctl(fd_, UI_SET_EVBIT, EV_SYN);
        ioctl(fd_, UI_SET_RELBIT, REL_X);  // relative axes make it count as a mouse
        ioctl(fd_, UI_SET_RELBIT, REL_Y);
        for (int i = 0; i < kKeyCount; i++) ioctl(fd_, UI_SET_KEYBIT, kKeys[i].linuxKey);
        for (int code : kMouseCodes) ioctl(fd_, UI_SET_KEYBIT, code);
        uinput_user_dev setup;
        memset(&setup, 0, sizeof setup);
        if (!createDevice(fd_, "SpacemouseHOTAS Virtual Keyboard", 0x5349, setup)) {
            ::close(fd_);
            fd_ = -1;
            failed_ = true;
            return false;
        }
        return true;
    }
} g_keyboard;

}  // namespace

bool UinputPad::open(int) {
    close();
    fd_ = openUinput();
    if (fd_ < 0) {
        snprintf(status_, sizeof status_, "No access to /dev/uinput");
        return false;
    }
    ioctl(fd_, UI_SET_EVBIT, EV_KEY);
    ioctl(fd_, UI_SET_EVBIT, EV_ABS);
    ioctl(fd_, UI_SET_EVBIT, EV_SYN);
    uinput_user_dev setup;
    memset(&setup, 0, sizeof setup);
    for (int code : kAxisCodes) {
        ioctl(fd_, UI_SET_ABSBIT, code);
        setup.absmin[code] = -32767;
        setup.absmax[code] = 32767;
    }
    for (int code : {ABS_HAT0X, ABS_HAT0Y}) {
        ioctl(fd_, UI_SET_ABSBIT, code);
        setup.absmin[code] = -1;
        setup.absmax[code] = 1;
    }
    for (int b = 0; b < kPadButtons; b++) ioctl(fd_, UI_SET_KEYBIT, joystickButtonCode(b));
    if (!createDevice(fd_, "SpacemouseHOTAS Virtual HOTAS", 0x5348, setup)) {
        snprintf(status_, sizeof status_, "uinput create failed (%s)", strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    sentButtons_ = 0;
    snprintf(status_, sizeof status_, "uinput device ready");
    return true;
}

void UinputPad::close() {
    if (fd_ >= 0) {
        ioctl(fd_, UI_DEV_DESTROY);
        ::close(fd_);
    }
    fd_ = -1;
}

bool UinputPad::send(const float* axes, uint32_t buttons, int hat) {
    if (fd_ < 0) return false;
    for (int i = 0; i < kPadAxes; i++) emit(fd_, EV_ABS, kAxisCodes[i], (int)lroundf(axes[i] * 32767.0f));
    static const int kHatX[8] = {0, 1, 1, 1, 0, -1, -1, -1}, kHatY[8] = {-1, -1, 0, 1, 1, 1, 0, -1};
    emit(fd_, EV_ABS, ABS_HAT0X, hat < 0 ? 0 : kHatX[hat]);
    emit(fd_, EV_ABS, ABS_HAT0Y, hat < 0 ? 0 : kHatY[hat]);
    uint32_t changedBits = buttons ^ sentButtons_;
    for (int b = 0; b < kPadButtons; b++)
        if ((changedBits >> b) & 1u) emit(fd_, EV_KEY, joystickButtonCode(b), (buttons >> b) & 1u);
    sentButtons_ = buttons;
    return emit(fd_, EV_SYN, SYN_REPORT, 0);
}

const char* UinputPad::help() const {
    return "Uses kernel uinput.\nNeeds read/write access to /dev/uinput and /dev/hidraw*: install the udev rule "
           "from packaging/linux.";
}

void injectKey(int vk, bool down) {
    if (const KeyDef* key = keyByVk(vk)) g_keyboard.press(key->linuxKey, down);
}

void injectMouseButton(int button, bool down) {
    if (button >= 0 && button < MOUSE_BUTTON_COUNT) g_keyboard.press(kMouseCodes[button], down);
}
