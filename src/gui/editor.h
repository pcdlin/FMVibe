#pragma once

#include "canvas.h"
#include "view.h"

#include <windows.h>

#include <bitset>
#include <cstdint>
#include <memory>

namespace fmvibe::gui {

// Win32 child window that hosts the View inside the host's plugin window. The
// view is drawn at a uniform scale, so the editor resizes with a fixed aspect ratio.
class Editor {
public:
    explicit Editor(Controller &controller);
    ~Editor();
    Editor(const Editor &) = delete;
    Editor &operator=(const Editor &) = delete;

    // Sizes are in physical pixels, as CLAP specifies for Win32.
    void size(uint32_t &width, uint32_t &height) const;
    // The height for `width` at the view's current proportions (the modulation panel may be collapsed).
    void fit(uint32_t &width, uint32_t &height) const;
    static void constrain(uint32_t &width, uint32_t &height, float viewHeight);
    bool setSize(uint32_t width, uint32_t height);
    bool attach(HWND parent);
    bool show(bool visible);
    // Opens the preset file dialog, after the current mouse gesture is over.
    void choosePreset(bool save);
    // A key the host's window took in, which the editor's keyboard hook passes on: true if it was
    // an undo or redo shortcut meant for this editor, which the host then doesn't see.
    bool shortcut(WPARAM key, bool up);
    // A message the host's loop took off the queue, which the editor's message hook passes on: true if it was
    // a key for a value being typed here, which the host then doesn't see.
    bool typedKey(const MSG &msg);

private:
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);
    void paint();
    void invalidate();
    void tick(); // a poll of the view, painting what changed: on the poll timer, or on mouse moves holding it off
    float scale() const;
    Point toView(int x, int y) const;
    void updateCursor();
    void contextMenu(int x, int y);
    void browsePreset(bool save);
    // The editor takes the keyboard while a value is typed, and hands it back after.
    void syncFocus();
    bool key(WPARAM key, LPARAM flags);

    Controller &controller_;
    View view_;
    Resources resources_;
    ComPtr<ID2D1HwndRenderTarget> target_;
    HWND hwnd_ = nullptr;
    bool resized_ = false; // the user changed the size, which new instances then open at
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool trackingLeave_ = false;
    ULONGLONG lastPoll_ = 0;       // when tick() last ran (GetTickCount64)
    bool hasKeyboard_ = false;     // taken for typing
    HWND keyboardFrom_ = nullptr;  // who had it before
    WPARAM shortcutKey_ = 0;       // the shortcut's key while it is down, to take its key-up as well
    std::bitset<256> typedKeys_;   // keys taken while typing, to take their key-ups as well
    // False once the editor is gone, for code that ran a modal loop meanwhile (see ModalGuard in editor.cpp).
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

} // namespace fmvibe::gui
