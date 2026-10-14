// Linux tray icon. Uses StatusNotifierItem over D-Bus (KDE, GNOME with the AppIndicator extension,
// most current panels) with libdbus loaded at runtime, and falls back to the XEmbed system tray.
// Right-click opens a small menu drawn by this app.
#pragma once
#include <X11/Xlib.h>

struct TrayActions {
    void (*toggleWindow)() = nullptr;
    void (*toggleOutput)() = nullptr;
    void (*quit)() = nullptr;
};

class Tray {
public:
    // Returns false if neither tray protocol is available.
    bool start(const TrayActions& actions, int uiScale);
    void setOutputEnabled(bool enabled);
    bool available() const;

    int dbusFd() const;                // -1 if D-Bus is not in use
    void processDbus();                // call when dbusFd() is readable
    bool handleXEvent(XEvent& event);  // true if the event belonged to the tray or its menu
};
