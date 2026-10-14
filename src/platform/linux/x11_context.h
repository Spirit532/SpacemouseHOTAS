// Shared X11 connection, set once by main and used by the font loader and the tray.
#pragma once
#include <X11/Xlib.h>

struct X11Context {
    Display* display = nullptr;
    int screen = 0;
    Window root = 0;
};

extern X11Context g_x11;
