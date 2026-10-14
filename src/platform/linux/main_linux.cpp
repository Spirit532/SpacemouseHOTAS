// Linux host: hidraw input, uinput output, Xlib settings window and tray. One thread that
// sleeps in poll() while nothing happens.
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "app.h"
#include "core/keys.h"
#include "platform/linux/system_linux.h"
#include "platform/linux/tray_linux.h"
#include "platform/linux/uinput_linux.h"
#include "platform/linux/x11_context.h"
#include "ui/icon.h"

X11Context g_x11;

namespace {

App g_app;
UinputPad g_pad;
Tray g_tray;
Window g_window = 0;
GC g_gc = nullptr;
Atom g_atomDeleteWindow = 0, g_atomWindowIcon = 0, g_atomWindowName = 0, g_atomUtf8 = 0;
int g_scale = 1;
UiInput g_input;
double g_lastPaint = 0;
bool g_paintPending = false;
bool g_trayShowsOutput = true;

struct HidDevice {
    int fd;
    std::string path;
};
std::vector<HidDevice> g_devices;
int g_inotifyFd = -1;

double now() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// hidraw -------------------------------------------------------------------------------------

void scanHidrawDevices() {
    DIR* dir = opendir("/dev");
    if (!dir) return;
    while (dirent* entry = readdir(dir)) {
        if (strncmp(entry->d_name, "hidraw", 6) != 0) continue;
        std::string path = std::string("/dev/") + entry->d_name;
        bool known = false;
        for (const HidDevice& d : g_devices) known |= d.path == path;
        if (known) continue;
        int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;  // no permission yet; inotify reports the udev ACL change
        hidraw_devinfo info;
        memset(&info, 0, sizeof info);
        if (ioctl(fd, HIDIOCGRAWINFO, &info) < 0 || !isSpaceMouse((uint16_t)info.vendor, (uint16_t)info.product)) {
            ::close(fd);
            continue;
        }
        char name[128] = {};
        ioctl(fd, HIDIOCGRAWNAME(sizeof name - 1), name);
        const char* shortName = strncmp(name, "3Dconnexion ", 12) ? name : name + 12;
        g_devices.push_back({fd, path});
        g_app.onDeviceArrived((uint16_t)info.vendor, (uint16_t)info.product, shortName);
    }
    closedir(dir);
}

void readHidrawDevice(size_t index) {
    uint8_t report[64];
    for (;;) {
        ssize_t length = read(g_devices[index].fd, report, sizeof report);
        if (length > 0) {
            g_app.onDeviceReport(report, (int)length);
            continue;
        }
        if (length < 0 && errno == EAGAIN) return;
        ::close(g_devices[index].fd);  // unplugged
        g_devices.erase(g_devices.begin() + index);
        g_app.onDeviceRemoved();
        return;
    }
}

// Window -------------------------------------------------------------------------------------

void setWindowIcon(Window window, bool outputEnabled) {
    std::vector<unsigned long> data;  // _NET_WM_ICON: width, height, ARGB pixels as longs
    for (int size : {16, 32, 48}) {
        std::vector<uint32_t> argb(size * size);
        makeIcon(argb.data(), size, outputEnabled);
        data.push_back(size);
        data.push_back(size);
        data.insert(data.end(), argb.begin(), argb.end());
    }
    XChangeProperty(g_x11.display, window, g_atomWindowIcon, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char*)data.data(), (int)data.size());
}

int scaleFromXftDpi() {
    if (g_app.profile.uiScale > 0) return g_app.profile.uiScale;
    const char* dpi = XGetDefault(g_x11.display, "Xft", "dpi");
    int scale = (int)lround((dpi ? atof(dpi) : 96.0) / 96.0);
    return scale < 1 ? 1 : scale;
}

void applyWindowSize() {
    int width = App::kWidth * g_scale, height = App::kHeight * g_scale;
    XSizeHints* hints = XAllocSizeHints();
    hints->flags = PMinSize | PMaxSize;
    hints->min_width = hints->max_width = width;
    hints->min_height = hints->max_height = height;
    XSetWMNormalHints(g_x11.display, g_window, hints);
    XFree(hints);
    XResizeWindow(g_x11.display, g_window, width, height);
}

void openWindow() {
    Display* display = g_x11.display;
    if (g_window) {
        XMapRaised(display, g_window);
        return;
    }
    g_scale = scaleFromXftDpi();
    g_window =
        XCreateSimpleWindow(display, g_x11.root, 0, 0, App::kWidth * g_scale, App::kHeight * g_scale, 0, 0, 0x1B1D21);
    applyWindowSize();
    XStoreName(display, g_window, "SpacemouseHOTAS");
    XChangeProperty(display, g_window, g_atomWindowName, g_atomUtf8, 8, PropModeReplace,
                    (const unsigned char*)"SpacemouseHOTAS", 15);
    XClassHint classHint = {(char*)"spacemousehotas", (char*)"SpacemouseHOTAS"};
    XSetClassHint(display, g_window, &classHint);
    XSetWMProtocols(display, g_window, &g_atomDeleteWindow, 1);
    setWindowIcon(g_window, g_app.profile.outputEnabled);
    XSelectInput(display, g_window,
                 ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | LeaveWindowMask |
                     StructureNotifyMask | KeyPressMask);
    XMapRaised(display, g_window);
    g_input = UiInput();
    g_paintPending = true;
}

void closeWindow() {
    if (!g_window) return;
    XDestroyWindow(g_x11.display, g_window);
    g_window = 0;
    g_app.releaseCanvas();
    if (g_app.configDirty) g_app.save();
}

void toggleWindow() {
    if (g_window)
        closeWindow();
    else
        openWindow();
}

void toggleOutput() { g_app.setOutputEnabled(!g_app.profile.outputEnabled); }

void requestQuit() { g_app.quitRequested = true; }

void paintWindow() {
    if (!g_window) return;
    bool again = g_app.drawFrame(g_input, g_scale);
    g_input.clearEdges();
    const Canvas& canvas = g_app.canvas();
    if (canvas.pixels) {
        Display* display = g_x11.display;
        XImage* image =
            XCreateImage(display, DefaultVisual(display, g_x11.screen), DefaultDepth(display, g_x11.screen), ZPixmap, 0,
                         (char*)canvas.pixels, canvas.width, canvas.height, 32, canvas.width * 4);
        if (image) {
            XPutImage(display, g_window, g_gc, image, 0, 0, 0, 0, canvas.width, canvas.height);
            image->data = nullptr;  // pixels belong to the canvas
            XDestroyImage(image);
        }
    }
    g_lastPaint = now();
    g_paintPending = again;
}

void setMousePosition(int x, int y) {
    g_input.mouseX = x / g_scale;
    g_input.mouseY = y / g_scale;
}

void handleWindowEvent(XEvent& event) {
    switch (event.type) {
    case Expose:
        if (event.xexpose.count == 0) g_paintPending = true;
        break;
    case MotionNotify:
        setMousePosition(event.xmotion.x, event.xmotion.y);
        paintWindow();
        break;
    case LeaveNotify:
        if (!g_input.leftDown) g_input.mouseX = g_input.mouseY = -1;
        paintWindow();
        break;
    case ButtonPress:
        setMousePosition(event.xbutton.x, event.xbutton.y);
        switch (event.xbutton.button) {
        case 1: g_input.leftDown = g_input.leftPressed = true; break;
        case 2: g_input.middlePressed = true; break;
        case 3: g_input.rightPressed = true; break;
        case 4: g_input.wheel++; break;
        case 5: g_input.wheel--; break;
        case 8: g_input.x1Pressed = true; break;
        case 9: g_input.x2Pressed = true; break;
        }
        paintWindow();
        break;
    case ButtonRelease:
        setMousePosition(event.xbutton.x, event.xbutton.y);
        if (event.xbutton.button == 1) {
            g_input.leftDown = false;
            g_input.leftReleased = true;
            paintWindow();
        }
        break;
    case KeyPress: {
        char typed[16] = {};
        KeySym keysym = 0;
        int length = XLookupString(&event.xkey, typed, sizeof typed - 1, &keysym, nullptr);
        for (int i = 0; i < length; i++)
            if (typed[i] >= 0x20 && typed[i] < 0x7F) g_input.text += typed[i];
        if (const KeyDef* key = keyByX11Keysym((uint32_t)XLookupKeysym(&event.xkey, 0))) g_input.key = key->vk;
        paintWindow();
        break;
    }
    case ClientMessage:
        if ((Atom)event.xclient.data.l[0] == g_atomDeleteWindow) {
            closeWindow();
            if (!g_tray.available()) g_app.quitRequested = true;  // nothing left to reopen it from
        }
        break;
    }
}

void processXEvents() {
    Display* display = g_x11.display;
    while (XPending(display)) {
        XEvent event;
        XNextEvent(display, &event);
        if (g_tray.handleXEvent(event)) continue;
        if (g_window && event.xany.window == g_window) handleWindowEvent(event);
    }
    if (g_trayShowsOutput != g_app.profile.outputEnabled) {
        g_trayShowsOutput = g_app.profile.outputEnabled;
        g_tray.setOutputEnabled(g_trayShowsOutput);
        if (g_window) setWindowIcon(g_window, g_trayShowsOutput);
    }
    if (g_window && scaleFromXftDpi() != g_scale) {  // UI scale setting changed
        g_scale = scaleFromXftDpi();
        applyWindowSize();
        g_paintPending = true;
    }
    if (g_window && (g_paintPending || (g_app.redrawNeeded && now() - g_lastPaint >= 0.033))) paintWindow();
    g_app.runDeferredActions();
    XFlush(display);
}

bool connectX11() {
    Display* display = XOpenDisplay(nullptr);
    if (!display) return false;
    g_x11.display = display;
    g_x11.screen = DefaultScreen(display);
    g_x11.root = RootWindow(display, g_x11.screen);
    g_gc = XCreateGC(display, g_x11.root, 0, nullptr);
    g_atomDeleteWindow = XInternAtom(display, "WM_DELETE_WINDOW", False);
    g_atomWindowIcon = XInternAtom(display, "_NET_WM_ICON", False);
    g_atomWindowName = XInternAtom(display, "_NET_WM_NAME", False);
    g_atomUtf8 = XInternAtom(display, "UTF8_STRING", False);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    bool showWindow = false;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--show")) showWindow = true;

    PlatformHooks hooks;
    hooks.injectKey = injectKey;
    hooks.injectMouseButton = injectMouseButton;
    hooks.setStartWithSystem = setStartWithLinux;
    hooks.startWithSystemLabel = "Start with Linux";
    g_app.init(&g_pad, hooks, configFilePath());
    g_trayShowsOutput = g_app.profile.outputEnabled;

    bool hasDisplay = connectX11();  // without X the mapping still runs, headless
    if (hasDisplay) {
        TrayActions actions;
        actions.toggleWindow = toggleWindow;
        actions.toggleOutput = toggleOutput;
        actions.quit = requestQuit;
        g_tray.start(actions, scaleFromXftDpi());
        g_tray.setOutputEnabled(g_app.profile.outputEnabled);
        if (showWindow || g_app.firstRun || g_app.profile.openWindowAtStart || !g_tray.available()) openWindow();
    }

    g_inotifyFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (g_inotifyFd >= 0) inotify_add_watch(g_inotifyFd, "/dev", IN_CREATE | IN_ATTRIB);
    scanHidrawDevices();
    g_app.update(now());

    std::vector<pollfd> fds;
    while (!g_app.quitRequested) {
        if (hasDisplay) processXEvents();
        if (g_app.quitRequested) break;

        int timeoutMs = -1;
        if (g_app.timedUpdatesNeeded) timeoutMs = 10;
        if (g_window && g_app.redrawNeeded) {
            int untilPaint = (int)((0.033 - (now() - g_lastPaint)) * 1000) + 1;
            if (untilPaint < 1) untilPaint = 1;
            if (timeoutMs < 0 || untilPaint < timeoutMs) timeoutMs = untilPaint;
        }

        fds.clear();
        int dbusIndex = -1, inotifyIndex = -1;
        if (hasDisplay) fds.push_back({ConnectionNumber(g_x11.display), POLLIN, 0});  // drained at the loop top
        if (g_tray.dbusFd() >= 0) {
            dbusIndex = (int)fds.size();
            fds.push_back({g_tray.dbusFd(), POLLIN, 0});
        }
        if (g_inotifyFd >= 0) {
            inotifyIndex = (int)fds.size();
            fds.push_back({g_inotifyFd, POLLIN, 0});
        }
        size_t firstDevice = fds.size();
        for (const HidDevice& d : g_devices) fds.push_back({d.fd, POLLIN, 0});
        if (poll(fds.data(), fds.size(), timeoutMs) < 0 && errno != EINTR) break;

        bool inputArrived = false;
        for (size_t i = g_devices.size(); i-- > 0;)
            if (fds[firstDevice + i].revents) {
                readHidrawDevice(i);
                inputArrived = true;
            }
        if (inotifyIndex >= 0 && fds[inotifyIndex].revents) {
            char events[4096];
            while (read(g_inotifyFd, events, sizeof events) > 0) {
            }
            scanHidrawDevices();
            inputArrived = true;
        }
        if (dbusIndex >= 0 && fds[dbusIndex].revents) g_tray.processDbus();
        if (inputArrived || g_app.timedUpdatesNeeded) g_app.update(now());
    }

    closeWindow();
    g_app.shutdown();
    for (const HidDevice& d : g_devices) ::close(d.fd);
    if (hasDisplay) XCloseDisplay(g_x11.display);
    return 0;
}
