#include "platform/linux/tray_linux.h"
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>
#include "platform/linux/x11_context.h"
#include "ui/canvas.h"
#include "ui/icon.h"
#include "ui/widgets.h"

namespace {

// libdbus-1 subset, resolved with dlopen so the binary neither links nor requires it -------

struct DBusConnection;
struct DBusMessage;
struct DBusError {  // public layout: name, message, then private fields
    const char* name;
    const char* message;
    unsigned char reserved[48];
};
struct DBusMessageIter {  // opaque, larger than the real struct on every ABI
    alignas(8) unsigned char storage[128];
};
typedef uint32_t dbus_bool_t;
typedef int (*DBusMessageFunction)(DBusConnection*, DBusMessage*, void*);
struct DBusObjectPathVTable {
    void (*unregister)(DBusConnection*, void*);
    DBusMessageFunction message;
    void (*reserved[4])(void*);
};

enum {
    BUS_SESSION = 0,
    HANDLER_HANDLED = 0,
    HANDLER_NOT_HANDLED = 1,
    DISPATCH_DATA_REMAINS = 0,
    NAME_PRIMARY_OWNER = 1
};
enum : int {
    TYPE_INVALID = 0,
    TYPE_BOOLEAN = 'b',
    TYPE_INT32 = 'i',
    TYPE_BYTE = 'y',
    TYPE_STRING = 's',
    TYPE_OBJECT_PATH = 'o',
    TYPE_ARRAY = 'a',
    TYPE_VARIANT = 'v',
    TYPE_STRUCT = 'r',
    TYPE_DICT_ENTRY = 'e'
};

struct DBusApi {
    void* library = nullptr;
    void (*errorInit)(DBusError*);
    void (*errorFree)(DBusError*);
    DBusConnection* (*busGet)(int, DBusError*);
    int (*requestName)(DBusConnection*, const char*, unsigned, DBusError*);
    dbus_bool_t (*registerObjectPath)(DBusConnection*, const char*, const DBusObjectPathVTable*, void*);
    dbus_bool_t (*getUnixFd)(DBusConnection*, int*);
    dbus_bool_t (*readWrite)(DBusConnection*, int);
    int (*dispatch)(DBusConnection*);
    dbus_bool_t (*send)(DBusConnection*, DBusMessage*, uint32_t*);
    DBusMessage* (*sendWithReplyAndBlock)(DBusConnection*, DBusMessage*, int, DBusError*);
    void (*flush)(DBusConnection*);
    void (*setExitOnDisconnect)(DBusConnection*, dbus_bool_t);
    DBusMessage* (*newMethodCall)(const char*, const char*, const char*, const char*);
    DBusMessage* (*newMethodReturn)(DBusMessage*);
    DBusMessage* (*newSignal)(const char*, const char*, const char*);
    DBusMessage* (*newError)(DBusMessage*, const char*, const char*);
    void (*unref)(DBusMessage*);
    dbus_bool_t (*isMethodCall)(DBusMessage*, const char*, const char*);
    dbus_bool_t (*getArgs)(DBusMessage*, DBusError*, int, ...);
    void (*iterInitAppend)(DBusMessage*, DBusMessageIter*);
    dbus_bool_t (*appendBasic)(DBusMessageIter*, int, const void*);
    dbus_bool_t (*openContainer)(DBusMessageIter*, int, const char*, DBusMessageIter*);
    dbus_bool_t (*closeContainer)(DBusMessageIter*, DBusMessageIter*);
    dbus_bool_t (*appendFixedArray)(DBusMessageIter*, int, const void*, int);

    bool load() {
        if (library) return true;
        void* lib = dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
        if (!lib) return false;
        bool ok = true;
        auto resolve = [&](auto& fn, const char* name) {
            fn = (std::remove_reference_t<decltype(fn)>)dlsym(lib, name);
            ok = ok && fn;
        };
        resolve(errorInit, "dbus_error_init");
        resolve(errorFree, "dbus_error_free");
        resolve(busGet, "dbus_bus_get");
        resolve(requestName, "dbus_bus_request_name");
        resolve(registerObjectPath, "dbus_connection_register_object_path");
        resolve(getUnixFd, "dbus_connection_get_unix_fd");
        resolve(readWrite, "dbus_connection_read_write");
        resolve(dispatch, "dbus_connection_dispatch");
        resolve(send, "dbus_connection_send");
        resolve(sendWithReplyAndBlock, "dbus_connection_send_with_reply_and_block");
        resolve(flush, "dbus_connection_flush");
        resolve(setExitOnDisconnect, "dbus_connection_set_exit_on_disconnect");
        resolve(newMethodCall, "dbus_message_new_method_call");
        resolve(newMethodReturn, "dbus_message_new_method_return");
        resolve(newSignal, "dbus_message_new_signal");
        resolve(newError, "dbus_message_new_error");
        resolve(unref, "dbus_message_unref");
        resolve(isMethodCall, "dbus_message_is_method_call");
        resolve(getArgs, "dbus_message_get_args");
        resolve(iterInitAppend, "dbus_message_iter_init_append");
        resolve(appendBasic, "dbus_message_iter_append_basic");
        resolve(openContainer, "dbus_message_iter_open_container");
        resolve(closeContainer, "dbus_message_iter_close_container");
        resolve(appendFixedArray, "dbus_message_iter_append_fixed_array");
        if (!ok) {
            dlclose(lib);
            return false;
        }
        library = lib;
        return true;
    }
} g_dbus;

// Shared state -----------------------------------------------------------------------------

const char* kItemPath = "/StatusNotifierItem";
const char* kItemInterface = "org.kde.StatusNotifierItem";

TrayActions g_actions;
bool g_outputEnabled = true;
int g_scale = 1;

DBusConnection* g_bus = nullptr;
bool g_statusNotifierActive = false;

Window g_xembedWindow = 0;
int g_xembedWidth = 22, g_xembedHeight = 22;
Atom g_atomTrayOpcode = 0, g_atomTraySelection = 0, g_atomManager = 0, g_atomXembedInfo = 0;

void runAction(void (*action)()) {
    if (action) action();
}

// Popup menu -------------------------------------------------------------------------------

struct MenuItem {
    const char* label;
    bool checked;
    void (*action)();
};

const int kMenuWidth = 160, kMenuItemHeight = 22;
Window g_menuWindow = 0;
Canvas g_menuCanvas;
std::vector<MenuItem> g_menuItems;
int g_menuHover = -1;
bool g_menuArmed = false;  // ignore the button release of the click that opened the menu

void drawMenu() {
    int height = (int)g_menuItems.size() * kMenuItemHeight + 4;
    if (!g_menuCanvas.resize(kMenuWidth, height, g_scale)) return;
    Canvas& c = g_menuCanvas;
    c.fillRect(0, 0, kMenuWidth, height, theme::panel);
    c.strokeRect(0, 0, kMenuWidth, height, theme::border);
    for (int i = 0; i < (int)g_menuItems.size(); i++) {
        int y = 2 + i * kMenuItemHeight;
        if (i == g_menuHover) c.fillRect(2, y, kMenuWidth - 4, kMenuItemHeight, theme::fieldHover);
        if (g_menuItems[i].checked) c.fillRect(10, y + 8, 6, 6, theme::good);
        c.text(24, y + 7, g_menuItems[i].label, theme::text);
    }
    Display* display = g_x11.display;
    XImage* image = XCreateImage(display, DefaultVisual(display, g_x11.screen), DefaultDepth(display, g_x11.screen),
                                 ZPixmap, 0, (char*)c.pixels, c.width, c.height, 32, c.width * 4);
    if (!image) return;
    XPutImage(display, g_menuWindow, DefaultGC(display, g_x11.screen), image, 0, 0, 0, 0, c.width, c.height);
    image->data = nullptr;  // pixels belong to the canvas
    XDestroyImage(image);
}

void closeMenu() {
    if (!g_menuWindow) return;
    XUngrabPointer(g_x11.display, CurrentTime);
    XUngrabKeyboard(g_x11.display, CurrentTime);
    XDestroyWindow(g_x11.display, g_menuWindow);
    g_menuWindow = 0;
    g_menuCanvas.release();
}

void showMenu(int x, int y) {
    closeMenu();
    g_menuItems = {
        {"Output enabled", g_outputEnabled, g_actions.toggleOutput},
        {"Open window", false, g_actions.toggleWindow},
        {"Quit", false, g_actions.quit},
    };
    Display* display = g_x11.display;
    int width = kMenuWidth * g_scale, height = ((int)g_menuItems.size() * kMenuItemHeight + 4) * g_scale;
    int screenW = DisplayWidth(display, g_x11.screen), screenH = DisplayHeight(display, g_x11.screen);
    if (x + width > screenW) x = screenW - width;
    if (y + height > screenH) y = y - height;  // panels at the bottom: open upward
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    XSetWindowAttributes attributes = {};
    attributes.override_redirect = True;
    attributes.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | KeyPressMask;
    g_menuWindow = XCreateWindow(display, g_x11.root, x, y, width, height, 0, CopyFromParent, InputOutput,
                                 CopyFromParent, CWOverrideRedirect | CWEventMask, &attributes);
    XMapRaised(display, g_menuWindow);
    XGrabPointer(display, g_menuWindow, True, ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync,
                 GrabModeAsync, None, None, CurrentTime);
    XGrabKeyboard(display, g_menuWindow, True, GrabModeAsync, GrabModeAsync, CurrentTime);
    g_menuHover = -1;
    g_menuArmed = false;
    drawMenu();
}

void showMenuAtPointer() {
    Window rootReturn, childReturn;
    int rootX = 0, rootY = 0, winX, winY;
    unsigned mask;
    XQueryPointer(g_x11.display, g_x11.root, &rootReturn, &childReturn, &rootX, &rootY, &winX, &winY, &mask);
    showMenu(rootX + 1, rootY + 1);
}

bool handleMenuEvent(XEvent& event) {
    if (!g_menuWindow || event.xany.window != g_menuWindow) return false;
    int width = kMenuWidth * g_scale, height = ((int)g_menuItems.size() * kMenuItemHeight + 4) * g_scale;
    auto itemAt = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= width || y >= height) return -1;
        int item = (y / g_scale - 2) / kMenuItemHeight;
        return item >= 0 && item < (int)g_menuItems.size() ? item : -1;
    };
    switch (event.type) {
    case Expose: drawMenu(); break;
    case MotionNotify: {
        g_menuArmed = true;
        int hover = itemAt(event.xmotion.x, event.xmotion.y);
        if (hover != g_menuHover) {
            g_menuHover = hover;
            drawMenu();
        }
        break;
    }
    case ButtonPress:
        if (itemAt(event.xbutton.x, event.xbutton.y) < 0)
            closeMenu();  // click outside
        else
            g_menuArmed = true;
        break;
    case ButtonRelease: {
        int item = itemAt(event.xbutton.x, event.xbutton.y);
        if (item >= 0 && g_menuArmed) {
            void (*action)() = g_menuItems[item].action;
            closeMenu();
            runAction(action);
        }
        break;
    }
    case KeyPress:
        if (XLookupKeysym(&event.xkey, 0) == XK_Escape) closeMenu();
        break;
    }
    return true;
}

// StatusNotifierItem -----------------------------------------------------------------------

void appendString(DBusMessageIter* iter, const char* value) { g_dbus.appendBasic(iter, TYPE_STRING, &value); }

// a(iiay): ARGB32 pixmaps in network byte order.
void appendPixmaps(DBusMessageIter* iter, bool includeIcon) {
    DBusMessageIter array;
    g_dbus.openContainer(iter, TYPE_ARRAY, "(iiay)", &array);
    if (includeIcon) {
        for (int size : {24, 48}) {
            std::vector<uint32_t> argb(size * size);
            makeIcon(argb.data(), size, g_outputEnabled);
            std::vector<uint8_t> bytes;
            bytes.reserve(argb.size() * 4);
            for (uint32_t p : argb)
                bytes.insert(bytes.end(), {(uint8_t)(p >> 24), (uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p});
            DBusMessageIter entry, data;
            int32_t dimension = size;
            g_dbus.openContainer(&array, TYPE_STRUCT, nullptr, &entry);
            g_dbus.appendBasic(&entry, TYPE_INT32, &dimension);
            g_dbus.appendBasic(&entry, TYPE_INT32, &dimension);
            g_dbus.openContainer(&entry, TYPE_ARRAY, "y", &data);
            const uint8_t* raw = bytes.data();
            g_dbus.appendFixedArray(&data, TYPE_BYTE, &raw, (int)bytes.size());
            g_dbus.closeContainer(&entry, &data);
            g_dbus.closeContainer(&array, &entry);
        }
    }
    g_dbus.closeContainer(iter, &array);
}

const char* const kPropertyNames[] = {"Category",
                                      "Id",
                                      "Title",
                                      "Status",
                                      "WindowId",
                                      "IconName",
                                      "IconPixmap",
                                      "OverlayIconName",
                                      "OverlayIconPixmap",
                                      "AttentionIconName",
                                      "AttentionIconPixmap",
                                      "AttentionMovieName",
                                      "ToolTip",
                                      "ItemIsMenu",
                                      "Menu"};

// Appends the property as a variant; false if the name is unknown.
bool appendProperty(DBusMessageIter* iter, const char* name) {
    DBusMessageIter variant;
    auto stringProperty = [&](const char* value) {
        g_dbus.openContainer(iter, TYPE_VARIANT, "s", &variant);
        appendString(&variant, value);
        g_dbus.closeContainer(iter, &variant);
    };
    if (!strcmp(name, "Category"))
        stringProperty("Hardware");
    else if (!strcmp(name, "Id"))
        stringProperty("spacemousehotas");
    else if (!strcmp(name, "Title"))
        stringProperty("SpacemouseHOTAS");
    else if (!strcmp(name, "Status"))
        stringProperty("Active");
    else if (!strcmp(name, "IconName") || !strcmp(name, "OverlayIconName") || !strcmp(name, "AttentionIconName") ||
             !strcmp(name, "AttentionMovieName"))
        stringProperty("");
    else if (!strcmp(name, "WindowId")) {
        int32_t none = 0;
        g_dbus.openContainer(iter, TYPE_VARIANT, "i", &variant);
        g_dbus.appendBasic(&variant, TYPE_INT32, &none);
        g_dbus.closeContainer(iter, &variant);
    } else if (!strcmp(name, "IconPixmap") || !strcmp(name, "OverlayIconPixmap") ||
               !strcmp(name, "AttentionIconPixmap")) {
        g_dbus.openContainer(iter, TYPE_VARIANT, "a(iiay)", &variant);
        appendPixmaps(&variant, !strcmp(name, "IconPixmap"));
        g_dbus.closeContainer(iter, &variant);
    } else if (!strcmp(name, "ToolTip")) {
        DBusMessageIter tooltip;
        g_dbus.openContainer(iter, TYPE_VARIANT, "(sa(iiay)ss)", &variant);
        g_dbus.openContainer(&variant, TYPE_STRUCT, nullptr, &tooltip);
        appendString(&tooltip, "");
        appendPixmaps(&tooltip, false);
        appendString(&tooltip, "SpacemouseHOTAS");
        appendString(&tooltip, g_outputEnabled ? "Output on" : "Output off");
        g_dbus.closeContainer(&variant, &tooltip);
        g_dbus.closeContainer(iter, &variant);
    } else if (!strcmp(name, "ItemIsMenu")) {
        dbus_bool_t no = 0;
        g_dbus.openContainer(iter, TYPE_VARIANT, "b", &variant);
        g_dbus.appendBasic(&variant, TYPE_BOOLEAN, &no);
        g_dbus.closeContainer(iter, &variant);
    } else if (!strcmp(name, "Menu")) {
        const char* path = "/NO_DBUSMENU";  // no exported menu: hosts call ContextMenu instead
        g_dbus.openContainer(iter, TYPE_VARIANT, "o", &variant);
        g_dbus.appendBasic(&variant, TYPE_OBJECT_PATH, &path);
        g_dbus.closeContainer(iter, &variant);
    } else {
        return false;
    }
    return true;
}

const char* kIntrospection =
    "<node>"
    "<interface name=\"org.kde.StatusNotifierItem\">"
    "<method name=\"Activate\"><arg type=\"i\" direction=\"in\"/><arg type=\"i\" direction=\"in\"/></method>"
    "<method name=\"SecondaryActivate\"><arg type=\"i\" direction=\"in\"/><arg type=\"i\" direction=\"in\"/></method>"
    "<method name=\"ContextMenu\"><arg type=\"i\" direction=\"in\"/><arg type=\"i\" direction=\"in\"/></method>"
    "<method name=\"Scroll\"><arg type=\"i\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/></method>"
    "<signal name=\"NewIcon\"/><signal name=\"NewToolTip\"/>"
    "</interface>"
    "</node>";

void sendReply(DBusConnection* bus, DBusMessage* reply) {
    if (!reply) return;
    g_dbus.send(bus, reply, nullptr);
    g_dbus.unref(reply);
}

int handleBusMessage(DBusConnection* bus, DBusMessage* message, void*) {
    auto isCall = [&](const char* interfaceName, const char* method) {
        return g_dbus.isMethodCall(message, interfaceName, method) != 0;
    };
    if (isCall(kItemInterface, "Activate")) {
        sendReply(bus, g_dbus.newMethodReturn(message));
        runAction(g_actions.toggleWindow);
    } else if (isCall(kItemInterface, "SecondaryActivate")) {
        sendReply(bus, g_dbus.newMethodReturn(message));
        runAction(g_actions.toggleOutput);
    } else if (isCall(kItemInterface, "ContextMenu")) {
        int32_t x = 0, y = 0;
        g_dbus.getArgs(message, nullptr, TYPE_INT32, &x, TYPE_INT32, &y, TYPE_INVALID);
        sendReply(bus, g_dbus.newMethodReturn(message));
        if (x > 0 || y > 0)
            showMenu(x, y);
        else
            showMenuAtPointer();
    } else if (isCall(kItemInterface, "Scroll")) {
        sendReply(bus, g_dbus.newMethodReturn(message));
    } else if (isCall("org.freedesktop.DBus.Properties", "Get")) {
        const char* interfaceName = "";
        const char* property = "";
        g_dbus.getArgs(message, nullptr, TYPE_STRING, &interfaceName, TYPE_STRING, &property, TYPE_INVALID);
        DBusMessage* reply = g_dbus.newMethodReturn(message);
        DBusMessageIter iter;
        g_dbus.iterInitAppend(reply, &iter);
        if (!appendProperty(&iter, property)) {
            g_dbus.unref(reply);
            reply = g_dbus.newError(message, "org.freedesktop.DBus.Error.UnknownProperty", property);
        }
        sendReply(bus, reply);
    } else if (isCall("org.freedesktop.DBus.Properties", "GetAll")) {
        DBusMessage* reply = g_dbus.newMethodReturn(message);
        DBusMessageIter iter, dictionary;
        g_dbus.iterInitAppend(reply, &iter);
        g_dbus.openContainer(&iter, TYPE_ARRAY, "{sv}", &dictionary);
        for (const char* name : kPropertyNames) {
            DBusMessageIter entry;
            g_dbus.openContainer(&dictionary, TYPE_DICT_ENTRY, nullptr, &entry);
            appendString(&entry, name);
            appendProperty(&entry, name);
            g_dbus.closeContainer(&dictionary, &entry);
        }
        g_dbus.closeContainer(&iter, &dictionary);
        sendReply(bus, reply);
    } else if (isCall("org.freedesktop.DBus.Introspectable", "Introspect")) {
        DBusMessage* reply = g_dbus.newMethodReturn(message);
        DBusMessageIter iter;
        g_dbus.iterInitAppend(reply, &iter);
        appendString(&iter, kIntrospection);
        sendReply(bus, reply);
    } else {
        return HANDLER_NOT_HANDLED;
    }
    g_dbus.flush(bus);
    return HANDLER_HANDLED;
}

bool startStatusNotifier() {
    if (!g_dbus.load()) return false;
    DBusError error;
    g_dbus.errorInit(&error);
    g_bus = g_dbus.busGet(BUS_SESSION, &error);
    if (!g_bus) {
        g_dbus.errorFree(&error);
        return false;
    }
    g_dbus.setExitOnDisconnect(g_bus, 0);
    char serviceName[64];
    snprintf(serviceName, sizeof serviceName, "org.kde.StatusNotifierItem-%d-1", (int)getpid());
    if (g_dbus.requestName(g_bus, serviceName, 0, &error) != NAME_PRIMARY_OWNER) {
        g_dbus.errorFree(&error);
        return false;
    }
    static const DBusObjectPathVTable vtable = {nullptr, handleBusMessage, {}};
    if (!g_dbus.registerObjectPath(g_bus, kItemPath, &vtable, nullptr)) return false;

    DBusMessage* call = g_dbus.newMethodCall("org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
                                             "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem");
    DBusMessageIter iter;
    g_dbus.iterInitAppend(call, &iter);
    appendString(&iter, serviceName);
    DBusMessage* reply = g_dbus.sendWithReplyAndBlock(g_bus, call, 1000, &error);
    g_dbus.unref(call);
    if (!reply) {  // no watcher: the desktop has no StatusNotifier host
        g_dbus.errorFree(&error);
        return false;
    }
    g_dbus.unref(reply);
    return true;
}

void emitItemSignal(const char* name) {
    DBusMessage* signal = g_dbus.newSignal(kItemPath, kItemInterface, name);
    if (!signal) return;
    g_dbus.send(g_bus, signal, nullptr);
    g_dbus.unref(signal);
}

// XEmbed system tray -----------------------------------------------------------------------

void drawXembedIcon() {
    if (!g_xembedWindow) return;
    Display* display = g_x11.display;
    int size = g_xembedWidth < g_xembedHeight ? g_xembedWidth : g_xembedHeight;
    if (size < 8) size = 8;
    std::vector<uint32_t> argb(size * size);
    makeIcon(argb.data(), size, g_outputEnabled);
    XClearWindow(display, g_xembedWindow);  // ParentRelative background shows the panel
    GC gc = DefaultGC(display, g_x11.screen);
    int offsetX = (g_xembedWidth - size) / 2, offsetY = (g_xembedHeight - size) / 2;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            uint32_t p = argb[y * size + x];
            if ((p >> 24) < 128) continue;
            XSetForeground(display, gc, p & 0xFFFFFF);
            XDrawPoint(display, g_xembedWindow, gc, offsetX + x, offsetY + y);
        }
}

bool dockXembed() {
    Display* display = g_x11.display;
    Window manager = XGetSelectionOwner(display, g_atomTraySelection);
    if (!manager) return false;
    if (!g_xembedWindow) {
        XSetWindowAttributes attributes = {};
        attributes.background_pixmap = ParentRelative;
        attributes.event_mask = ExposureMask | ButtonPressMask | StructureNotifyMask;
        g_xembedWindow = XCreateWindow(display, g_x11.root, 0, 0, g_xembedWidth, g_xembedHeight, 0, CopyFromParent,
                                       InputOutput, CopyFromParent, CWBackPixmap | CWEventMask, &attributes);
        unsigned long info[2] = {0, 1};  // XEmbed version 0, mapped
        XChangeProperty(display, g_xembedWindow, g_atomXembedInfo, g_atomXembedInfo, 32, PropModeReplace,
                        (unsigned char*)info, 2);
        XStoreName(display, g_xembedWindow, "SpacemouseHOTAS");
    }
    XEvent request = {};
    request.xclient.type = ClientMessage;
    request.xclient.window = manager;
    request.xclient.message_type = g_atomTrayOpcode;
    request.xclient.format = 32;
    request.xclient.data.l[0] = CurrentTime;
    request.xclient.data.l[1] = 0;  // SYSTEM_TRAY_REQUEST_DOCK
    request.xclient.data.l[2] = (long)g_xembedWindow;
    XSendEvent(display, manager, False, NoEventMask, &request);
    XFlush(display);
    return true;
}

}  // namespace

bool Tray::start(const TrayActions& actions, int uiScale) {
    g_actions = actions;
    g_scale = uiScale < 1 ? 1 : uiScale;
    Display* display = g_x11.display;
    char selection[32];
    snprintf(selection, sizeof selection, "_NET_SYSTEM_TRAY_S%d", g_x11.screen);
    g_atomTraySelection = XInternAtom(display, selection, False);
    g_atomTrayOpcode = XInternAtom(display, "_NET_SYSTEM_TRAY_OPCODE", False);
    g_atomManager = XInternAtom(display, "MANAGER", False);
    g_atomXembedInfo = XInternAtom(display, "_XEMBED_INFO", False);
    XSelectInput(display, g_x11.root, StructureNotifyMask);  // tray manager announcements

    g_statusNotifierActive = startStatusNotifier();
    if (!g_statusNotifierActive) dockXembed();
    return available();
}

bool Tray::available() const { return g_statusNotifierActive || g_xembedWindow != 0; }

void Tray::setOutputEnabled(bool enabled) {
    if (enabled == g_outputEnabled) return;
    g_outputEnabled = enabled;
    if (g_statusNotifierActive) {
        emitItemSignal("NewIcon");
        emitItemSignal("NewToolTip");
        g_dbus.flush(g_bus);
    }
    drawXembedIcon();
}

int Tray::dbusFd() const {
    int fd = -1;
    if (g_statusNotifierActive) g_dbus.getUnixFd(g_bus, &fd);
    return fd;
}

void Tray::processDbus() {
    if (!g_statusNotifierActive) return;
    g_dbus.readWrite(g_bus, 0);
    while (g_dbus.dispatch(g_bus) == DISPATCH_DATA_REMAINS) {
    }
}

bool Tray::handleXEvent(XEvent& event) {
    if (handleMenuEvent(event)) return true;
    if (g_xembedWindow && event.xany.window == g_xembedWindow) {
        switch (event.type) {
        case Expose: drawXembedIcon(); break;
        case ConfigureNotify:
            g_xembedWidth = event.xconfigure.width;
            g_xembedHeight = event.xconfigure.height;
            drawXembedIcon();
            break;
        case ButtonPress:
            if (event.xbutton.button == 1)
                runAction(g_actions.toggleWindow);
            else if (event.xbutton.button == 2)
                runAction(g_actions.toggleOutput);
            else if (event.xbutton.button == 3)
                showMenu(event.xbutton.x_root + 1, event.xbutton.y_root + 1);
            break;
        case DestroyNotify: g_xembedWindow = 0; break;
        }
        return true;
    }
    if (event.xany.window == g_x11.root) {
        // A tray manager started after us.
        if (!g_statusNotifierActive && event.type == ClientMessage && event.xclient.message_type == g_atomManager &&
            (Atom)event.xclient.data.l[1] == g_atomTraySelection)
            dockXembed();
        return true;
    }
    return false;
}
