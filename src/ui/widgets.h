// Immediate-mode widgets drawn on a Canvas. One dark theme.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "ui/canvas.h"

namespace theme {
const uint32_t background = 0x1B1D21, panel = 0x24272C, field = 0x2E3238, fieldHover = 0x383D45, fieldActive = 0x434955;
const uint32_t border = 0x3C424B, text = 0xD8DBE0, dim = 0x8B919B, faint = 0x50565F;
const uint32_t input = 0x4AA3FF, output = 0xFFA53D, good = 0x4CC979, bad = 0xE0565B, warn = 0xE0B84C;
}  // namespace theme

// Mouse and keyboard state for one frame. Edge flags and text cover the time since the last frame.
struct UiInput {
    int mouseX = -1, mouseY = -1;
    bool leftDown = false;
    bool leftPressed = false, leftReleased = false, rightPressed = false;
    bool middlePressed = false, x1Pressed = false, x2Pressed = false;
    int wheel = 0;     // notches, + = up
    int key = 0;       // Windows virtual-key code of a key pressed, 0 = none
    std::string text;  // printable characters typed

    void clearEdges() {
        leftPressed = leftReleased = rightPressed = middlePressed = x1Pressed = x2Pressed = false;
        wheel = 0;
        key = 0;
        text.clear();
    }
};

typedef const char* (*ItemNameFn)(int value, void* context);

struct DropdownStyle {
    bool arrowOnly = false;  // draw only the arrow button
    int listWidth = 0;       // 0 = widget width
    uint32_t color = 0;      // 0 = current accent
};

class Ui {
public:
    UiInput input;         // what widgets see: masked while an open list covers the mouse
    bool changed = false;  // some widget changed a value this frame
    uint32_t accent = theme::input;
    bool disabled = false;

    void begin(Canvas& canvas, const UiInput& frameInput);
    void end();

    bool hovered(int x, int y, int w, int h) const {
        return input.mouseX >= x && input.mouseY >= y && input.mouseX < x + w && input.mouseY < y + h;
    }
    // Tooltip for the widget drawn last, shown while it is hovered.
    void hint(const char* text, uint32_t borderColor = theme::dim);
    bool hasTextFocus() const { return focusId_ != 0; }

    void panel(int x, int y, int w, int h, const char* title, uint32_t titleColor);
    bool button(uintptr_t id, int x, int y, int w, int h, const char* label, bool on = false,
                uint32_t onColor = theme::input);
    bool checkbox(uintptr_t id, int x, int y, const char* label, bool* value);
    // Drag sets, wheel nudges, right-click resets to `reset`.
    bool slider(uintptr_t id, int x, int y, int w, int h, float* value, float lo, float hi, float reset,
                const char* format);
    // Values lo..lo+count-1 in a scrollable list. A pick lands in end(), so read *value after it.
    bool dropdown(uintptr_t id, int x, int y, int w, int h, int* value, int lo, int count, ItemNameFn name,
                  void* context, const DropdownStyle& style = DropdownStyle());
    // Single-line text input. Returns true when Enter is pressed while focused.
    bool textField(uintptr_t id, int x, int y, int w, int h, std::string& text, const char* placeholder);
    // List under a text field. Returns the item clicked in the previous frame, or -1.
    int suggestionList(uintptr_t id, int x, int y, int w, const std::vector<std::string>& items, int highlighted);

private:
    enum ListKind { LIST_NONE, LIST_DROPDOWN, LIST_SUGGESTIONS };
    struct OpenList {
        ListKind kind = LIST_NONE;
        uintptr_t id = 0;
        int x = 0, y = 0, w = 0, rows = 0;
        int ownerX = 0, ownerY = 0, ownerW = 0, ownerH = 0;
        int* value = nullptr;  // dropdown target
        int lo = 0, count = 0, scroll = 0, highlighted = -1;
        int picked = -1;  // suggestion clicked in end()
        uint32_t color = 0;
        bool drawnThisFrame = false;
        std::vector<std::string> items;
        int height() const { return rows * 18 + 2; }
    };

    Canvas* canvas_ = nullptr;
    UiInput raw_;
    uintptr_t hotId_ = 0, activeId_ = 0, focusId_ = 0;
    bool lastHovered_ = false;
    std::string tip_;
    uint32_t tipColor_ = theme::dim;
    OpenList list_;

    bool insideList(int x, int y) const;
    void openList(ListKind kind, uintptr_t id, int x, int y, int w, int count, int listWidth, bool alignRight);
    void drawList();
    void drawTooltip();
    void drawArrow(int cx, int cy, uint32_t color);
    bool press(uintptr_t id, bool live);  // tracks press/release; returns true on click
};

// Widget ids: hash of a stable address or name plus an optional index.
inline uintptr_t widgetId(const void* p, int index = 0) { return (uintptr_t)p * 8 + (uintptr_t)index; }
inline uintptr_t widgetId(const char* name, int index = 0) {
    uintptr_t h = 1469598103u;
    while (*name) h = (h ^ (unsigned char)*name++) * 16777619u;
    return (h ^ (uintptr_t)index) * 16777619u | 1;
}
