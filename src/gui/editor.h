#pragma once

#include <vector>

#include "canvas.h"
#include "view.h"

#include <windows.h>

#include <bitset>
#include <cstdint>
#include <memory>

namespace fmvibe::gui {

class FileDrop;

// Opens a folder in the system's file browser.
void showFolder(const std::string &utf8);

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
    // Likewise, the file dialog that imports a table or sample into an operator, and the folder dialog that adds a
    // folder to the wavetable library.
    void chooseOperatorFile(int op, DataKind kind);
    void chooseTableFolder();
    // A key the host's window took in, which the editor's keyboard hook passes on: true if it was
    // an undo or redo shortcut meant for this editor, which the host then doesn't see.
    bool shortcut(WPARAM key, bool up);
    // A message the host's loop took off the queue, which the editor's message hook passes on: true if it was
    // a key for a value being typed here, which the host then doesn't see.
    bool typedKey(const MSG &msg);
    // The whole editor painted again: the layout changed under it (a state loaded), which its poll doesn't watch.
    void repaint() { invalidate(); }

private:
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);
    void paint();
    void invalidate();
    // Only where these show (the view's coordinates), each apart: see View::changedAreas().
    void invalidate(const std::vector<Rect> &areas);
    void tick(); // a poll of the view, painting what changed: on the poll timer, or on mouse moves holding it off
    float scale() const;
    Point toView(int x, int y) const;
    // The window's DPI, once it has one: where it differs from the one its size was worked out for (the system's,
    // before the host gave it a parent), the host is asked for the size that keeps the editor's zoom at it.
    void followDpi();
    void updateCursor();
    void contextMenu(int x, int y);
    void browsePreset(bool save);
    void browseOperatorFile(int op, DataKind kind);
    void browseTableFolder();
    std::wstring lastImportFolder_; // where the import dialog was last left
    // Files dragged in from outside (the system's file browser, a host's): an OLE drop target on the window (see
    // FileDrop in editor.cpp), or WM_DROPFILES where OLE can't be had. The view lights where the file would land; a
    // drop is imported once the drag's loop has returned (kDropMessage), the file read here.
    friend class FileDrop;
    bool fileOver(const POINTL *screen); // nullptr once the file has left: true if it would land there
    void fileDropped(POINTL screen, const std::wstring &path);
    void importFile(Point at, const std::wstring &path);
    FileDrop *fileDrop_ = nullptr;
    bool oleInitialized_ = false;
    bool dropRegistered_ = false;
    std::wstring droppedPath_;
    Point droppedAt_;
    // The editor takes the keyboard while a value is typed or a dialog is open, and hands it back after.
    bool wantsKeyboard() const { return view_.typing() || view_.dialogOpen(); }
    void syncFocus();
    bool key(WPARAM key, LPARAM flags);

    Controller &controller_;
    View view_;
    Resources resources_;
    ComPtr<ID2D1HwndRenderTarget> target_;
    HWND hwnd_ = nullptr;
    bool resized_ = false; // the user changed the size, which new instances then open at
    float dpiScale_ = 1.0f; // the DPI the size is worked out for, over 96: the system's, then the window's
    uint32_t dpiWidth_ = 0; // the width followDpi() asked the host for, until it sets it: the zoom stays as it was
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool trackingLeave_ = false;
    bool touchPan_ = false;        // two fingers panning what scrolls under them (WM_GESTURE)
    ULONGLONG lastPoll_ = 0;       // when tick() last ran (GetTickCount64)
    bool hasKeyboard_ = false;     // taken for typing
    HWND keyboardFrom_ = nullptr;  // who had it before
    WPARAM shortcutKey_ = 0;       // the shortcut's key while it is down, to take its key-up as well
    std::bitset<256> typedKeys_;   // keys taken while typing, to take their key-ups as well
    // False once the editor is gone, for code that ran a modal loop meanwhile (see ModalGuard in editor.cpp).
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

} // namespace fmvibe::gui
