#include "ui/widgets.h"
#include <cmath>
#include <cstdio>
#include "core/keys.h"

using namespace theme;

void Ui::begin(Canvas& canvas, const UiInput& frameInput) {
    canvas_ = &canvas;
    raw_ = input = frameInput;
    hotId_ = 0;
    changed = false;
    tip_.clear();
    lastHovered_ = false;
    accent = theme::input;
    disabled = false;
    if (list_.kind == LIST_NONE) return;
    list_.drawnThisFrame = false;
    bool onOwner = raw_.mouseX >= list_.ownerX && raw_.mouseX < list_.ownerX + list_.ownerW &&
                   raw_.mouseY >= list_.ownerY && raw_.mouseY < list_.ownerY + list_.ownerH;
    if (insideList(raw_.mouseX, raw_.mouseY)) {
        // The list sits on top: hide the mouse from the widgets underneath.
        input.mouseX = input.mouseY = -1;
        input.leftPressed = input.leftReleased = input.rightPressed = input.middlePressed = false;
        input.wheel = 0;
    } else if (list_.kind == LIST_DROPDOWN && !onOwner && (raw_.leftPressed || raw_.rightPressed)) {
        list_.kind = LIST_NONE;  // click outside closes the list and is swallowed
        input.leftPressed = input.rightPressed = false;
    }
}

void Ui::end() {
    if (list_.kind != LIST_NONE && !list_.drawnThisFrame) list_.kind = LIST_NONE;  // owner went away
    if (list_.kind != LIST_NONE) drawList();
    if (!tip_.empty() && list_.kind == LIST_NONE) drawTooltip();
    if (!raw_.leftDown) activeId_ = 0;
}

bool Ui::insideList(int x, int y) const {
    return list_.kind != LIST_NONE && x >= list_.x && x < list_.x + list_.w && y >= list_.y &&
           y < list_.y + list_.height();
}

void Ui::hint(const char* text, uint32_t borderColor) {
    if (lastHovered_ && text) {
        tip_ = text;
        tipColor_ = borderColor;
    }
}

bool Ui::press(uintptr_t id, bool live) {
    if (live) hotId_ = id;
    if (live && input.leftPressed) activeId_ = id;
    return live && input.leftReleased && activeId_ == id;
}

void Ui::openList(ListKind kind, uintptr_t id, int x, int y, int w, int count, int listWidth, bool alignRight) {
    list_.kind = kind;
    list_.id = id;
    list_.count = count;
    list_.w = listWidth > 0 ? listWidth : w;
    list_.rows = count < 16 ? count : 16;
    list_.x = alignRight ? x + w - list_.w : x;
    if (list_.x < 2) list_.x = 2;
    list_.picked = -1;
}

void Ui::drawList() {
    Canvas& c = *canvas_;
    OpenList& l = list_;
    int h = l.height();
    bool inside = insideList(raw_.mouseX, raw_.mouseY);
    int maxScroll = l.count - l.rows;
    if (inside && raw_.wheel) {
        l.scroll -= raw_.wheel * 3;
        if (l.scroll > maxScroll) l.scroll = maxScroll;
        if (l.scroll < 0) l.scroll = 0;
    }
    c.fillRect(l.x + 3, l.y + 3, l.w, h, 0x101113);  // shadow
    c.fillRect(l.x, l.y, l.w, h, theme::panel);
    c.strokeRect(l.x, l.y, l.w, h, l.color);
    bool scrollbar = l.count > l.rows;
    int itemW = l.w - 2 - (scrollbar ? 6 : 0);
    int current = l.kind == LIST_DROPDOWN ? *l.value - l.lo : l.highlighted;
    for (int row = 0; row < l.rows && l.scroll + row < l.count; row++) {
        int i = l.scroll + row;
        int ry = l.y + 1 + row * 18;
        bool rowHovered = inside && raw_.mouseY >= ry && raw_.mouseY < ry + 18 && raw_.mouseX < l.x + 1 + itemW;
        if (i == current) c.fillRect(l.x + 1, ry, itemW, 18, blendColor(theme::panel, l.color, 70));
        if (rowHovered) c.fillRect(l.x + 1, ry, itemW, 18, blendColor(theme::panel, l.color, 120));
        c.textClipped(l.x + 7, ry + 5, itemW - 10, l.items[i].c_str(), text);
    }
    if (scrollbar) {
        int track = h - 2, knob = track * l.rows / l.count;
        if (knob < 10) knob = 10;
        int knobY = l.y + 1 + (track - knob) * l.scroll / (maxScroll > 0 ? maxScroll : 1);
        c.fillRect(l.x + l.w - 6, l.y + 1, 5, track, field);
        c.fillRect(l.x + l.w - 6, knobY, 5, knob, dim);
    }
    if (inside && raw_.leftPressed && raw_.mouseX < l.x + 1 + itemW) {
        int i = l.scroll + (raw_.mouseY - l.y - 1) / 18;
        if (i >= 0 && i < l.count) {
            if (l.kind == LIST_DROPDOWN) {
                if (*l.value != l.lo + i) changed = true;
                *l.value = l.lo + i;
            } else {
                l.picked = i;
                changed = true;
                return;  // stays open; the owner reads `picked` next frame
            }
        }
        l.kind = LIST_NONE;
    }
}

void Ui::drawTooltip() {
    Canvas& c = *canvas_;
    int w = c.textWidth(tip_.c_str()) + 12;
    int x = raw_.mouseX + 14, y = raw_.mouseY + 18;
    if (x + w > c.logicalWidth() - 2) x = c.logicalWidth() - 2 - w;
    if (y + 18 > c.logicalHeight() - 2) y = raw_.mouseY - 22;
    c.fillRect(x, y, w, 18, 0x15171A);
    c.strokeRect(x, y, w, 18, tipColor_);
    c.text(x + 6, y + 5, tip_.c_str(), text);
}

void Ui::drawArrow(int cx, int cy, uint32_t color) {
    for (int r = 0; r < 4; r++) canvas_->fillRect(cx - 3 + r, cy - 1 + r, 7 - 2 * r, 1, color);
}

void Ui::panel(int x, int y, int w, int h, const char* title, uint32_t titleColor) {
    Canvas& c = *canvas_;
    c.fillRect(x, y, w, h, theme::panel);
    c.strokeRect(x, y, w, h, border);
    if (!title) return;
    c.fillRect(x + 1, y + 1, w - 2, 16, blendColor(theme::panel, titleColor, 40));
    c.fillRect(x + 1, y + 1, 3, 16, titleColor);
    c.text(x + 9, y + 5, title, text, true);
}

bool Ui::button(uintptr_t id, int x, int y, int w, int h, const char* label, bool on, uint32_t onColor) {
    Canvas& c = *canvas_;
    lastHovered_ = hovered(x, y, w, h);
    bool live = lastHovered_ && !disabled;
    bool clicked = press(id, live);
    uint32_t fill = on ? blendColor(field, onColor, 110)
                       : (activeId_ == id && input.leftDown ? fieldActive : (live ? fieldHover : field));
    c.fillRect(x, y, w, h, fill);
    c.strokeRect(x, y, w, h, on ? onColor : border);
    int labelW = c.textWidth(label);
    c.textClipped(x + (labelW < w - 6 ? (w - labelW) / 2 : 3), y + (h - 8) / 2, w - 6, label, disabled ? faint : text);
    if (clicked) changed = true;
    return clicked;
}

bool Ui::checkbox(uintptr_t id, int x, int y, const char* label, bool* value) {
    Canvas& c = *canvas_;
    lastHovered_ = hovered(x, y, 20 + c.textWidth(label), 14);
    bool live = lastHovered_ && !disabled;
    bool clicked = press(id, live);
    if (clicked) {
        *value = !*value;
        changed = true;
    }
    uint32_t mark = disabled ? faint : accent;
    c.fillRect(x, y, 14, 14, live ? fieldHover : field);
    c.strokeRect(x, y, 14, 14, *value ? mark : border);
    if (*value) c.fillRect(x + 4, y + 4, 6, 6, mark);
    uint32_t labelColor = disabled ? faint : (*value && accent != theme::input ? accent : text);
    c.text(x + 20, y + 3, label, labelColor);
    return clicked;
}

bool Ui::drag(uintptr_t id, int x, int y, int w, int h) {
    lastHovered_ = hovered(x, y, w, h);
    press(id, lastHovered_ && !disabled);
    return activeId_ == id && input.leftDown && !disabled;
}

bool Ui::slider(uintptr_t id, int x, int y, int w, int h, float* value, float lo, float hi, float reset,
                const char* format, float snap) {
    Canvas& c = *canvas_;
    lastHovered_ = hovered(x, y, w, h);
    bool live = lastHovered_ && !disabled;
    float old = *value;
    press(id, live);
    if (activeId_ == id && input.leftDown && !disabled) {
        float t = (float)(input.mouseX - x - 1) / (float)(w - 3);
        *value = lo + (t < 0 ? 0 : (t > 1 ? 1 : t)) * (hi - lo);
        if (input.shiftDown && snap > 0) *value = roundf(*value / snap) * snap;
    }
    if (live && input.wheel) {
        if (input.shiftDown && snap > 0)
            *value = roundf(*value / snap + input.wheel) * snap;
        else
            *value += input.wheel * (hi - lo) / 100.0f;
    }
    if (live && input.rightPressed) *value = reset;
    if (*value < lo) *value = lo;
    if (*value > hi) *value = hi;

    uint32_t mark = disabled ? faint : accent;
    bool tinted = accent != theme::input && !disabled;
    c.fillRect(x, y, w, h, activeId_ == id ? fieldActive : (live ? fieldHover : field));
    c.strokeRect(x, y, w, h, tinted ? blendColor(border, accent, 150) : border);
    float origin = lo < 0 && hi > 0 ? 0.0f : lo;  // bipolar ranges fill from zero
    int originX = x + 1 + (int)((origin - lo) / (hi - lo) * (w - 3));
    int valueX = x + 1 + (int)((*value - lo) / (hi - lo) * (w - 3));
    int fillFrom = originX < valueX ? originX : valueX, fillTo = originX < valueX ? valueX : originX;
    c.fillRect(fillFrom, y + 1, fillTo - fillFrom + 1, h - 2, blendColor(field, mark, 90));
    c.fillRect(valueX, y + 1, 1, h - 2, mark);
    char label[32];
    snprintf(label, sizeof label, format, *value);
    c.text(x + (w - c.textWidth(label)) / 2, y + (h - 8) / 2, label, disabled ? faint : text);
    if (*value != old) changed = true;
    return *value != old;
}

bool Ui::dropdown(uintptr_t id, int x, int y, int w, int h, int* value, int lo, int count, ItemNameFn name,
                  void* context, const DropdownStyle& style) {
    Canvas& c = *canvas_;
    lastHovered_ = hovered(x, y, w, h);
    bool live = lastHovered_ && !disabled;
    int old = *value;
    bool open = list_.kind == LIST_DROPDOWN && list_.id == id;
    uint32_t color = style.color ? style.color : accent;
    if (press(id, live)) {
        if (open) {
            list_.kind = LIST_NONE;
            open = false;
        } else {
            openList(LIST_DROPDOWN, id, x, y, w, count, style.listWidth, style.arrowOnly);
            list_.value = value;
            list_.lo = lo;
            list_.color = color;
            list_.y = y + h + 1;
            if (list_.y + list_.height() > c.logicalHeight() - 2) list_.y = y - list_.height() - 1;
            if (list_.y < 2) list_.y = 2;
            list_.scroll = (*value - lo) - list_.rows / 2;
            if (list_.scroll > count - list_.rows) list_.scroll = count - list_.rows;
            if (list_.scroll < 0) list_.scroll = 0;
            open = true;
        }
    }
    if (live && input.wheel && !open && !style.arrowOnly) {
        *value -= input.wheel > 0 ? 1 : -1;
        if (*value < lo) *value = lo;
        if (*value > lo + count - 1) *value = lo + count - 1;
    }
    if (open) {
        list_.ownerX = x;
        list_.ownerY = y;
        list_.ownerW = w;
        list_.ownerH = h;
        list_.drawnThisFrame = true;
        list_.items.resize(count);
        for (int i = 0; i < count; i++) list_.items[i] = name(lo + i, context);
    }

    if (style.arrowOnly) {
        c.fillRect(x, y, w, h, blendColor(field, color, open ? 150 : (live ? 110 : 70)));
        c.strokeRect(x, y, w, h, color);
        drawArrow(x + w / 2, y + h / 2 - 1, text);
    } else {
        bool tinted = accent != theme::input && !disabled;
        uint32_t arrowColor = disabled ? faint : (style.color || tinted ? color : dim);
        c.fillRect(x, y, w, h, open ? fieldActive : (live ? fieldHover : field));
        c.strokeRect(x, y, w, h, open ? arrowColor : (tinted ? blendColor(border, accent, 150) : border));
        c.textClipped(x + 6, y + (h - 8) / 2, w - 22, name(*value, context), disabled ? faint : text);
        drawArrow(x + w - 9, y + h / 2 - 1, arrowColor);
    }
    if (*value != old) changed = true;
    return *value != old;
}

bool Ui::textField(uintptr_t id, int x, int y, int w, int h, std::string& text, const char* placeholder) {
    Canvas& c = *canvas_;
    lastHovered_ = hovered(x, y, w, h);
    bool live = lastHovered_ && !disabled;
    if (live && input.leftPressed)
        focusId_ = id;
    else if (input.leftPressed && focusId_ == id)
        focusId_ = 0;  // clicked elsewhere
    bool focused = focusId_ == id;
    bool submitted = false;
    if (focused) {
        for (char ch : input.text)
            if (ch >= 0x20 && ch < 0x7F && text.size() < 64) text += ch;
        if (input.key == VK_CODE_BACKSPACE && !text.empty()) text.pop_back();
        if (input.key == VK_CODE_ESCAPE) focusId_ = 0;
        if (input.key == VK_CODE_ENTER) submitted = true;
        if (!input.text.empty() || input.key) changed = true;
    }
    c.fillRect(x, y, w, h, focused ? fieldActive : (live ? fieldHover : field));
    c.strokeRect(x, y, w, h, focused ? accent : border);
    int textY = y + (h - 8) / 2;
    if (text.empty() && !focused) {
        c.textClipped(x + 6, textY, w - 12, placeholder, faint);
    } else {
        int end = c.textClipped(x + 6, textY, w - 12, text.c_str(), theme::text);
        if (focused) c.fillRect(end + 1, y + 3, 1, h - 6, theme::text);
    }
    return submitted;
}

int Ui::suggestionList(uintptr_t id, int x, int y, int w, const std::vector<std::string>& items, int highlighted) {
    bool open = list_.kind == LIST_SUGGESTIONS && list_.id == id;
    int picked = open ? list_.picked : -1;
    if (items.empty()) {
        if (open) list_.kind = LIST_NONE;
        return picked;
    }
    if (!open) openList(LIST_SUGGESTIONS, id, x, y, w, (int)items.size(), w, false);
    list_.count = (int)items.size();
    list_.rows = list_.count < 6 ? list_.count : 6;
    list_.x = x;
    list_.y = y;
    list_.w = w;
    list_.color = accent;
    list_.items = items;
    list_.highlighted = highlighted;
    if (highlighted >= 0) {  // keep the highlighted row visible
        if (highlighted < list_.scroll) list_.scroll = highlighted;
        if (highlighted >= list_.scroll + list_.rows) list_.scroll = highlighted - list_.rows + 1;
    }
    if (list_.scroll > list_.count - list_.rows) list_.scroll = list_.count - list_.rows;
    if (list_.scroll < 0) list_.scroll = 0;
    list_.ownerX = list_.ownerY = list_.ownerW = list_.ownerH = 0;
    list_.picked = -1;
    list_.drawnThisFrame = true;
    return picked;
}
