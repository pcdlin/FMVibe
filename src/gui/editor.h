#pragma once

// The editor window: a child window of the host's, hosting the View. On Windows it is a Win32 child drawn through
// Direct2D (editor.cpp); on Linux an X11 child drawn through Cairo (editor_x11.cpp), with the dialogs the desktop
// offers (zenity, else kdialog). Either draws the view at a uniform scale, so the editor keeps its aspect ratio.
//
// Text the editor shows itself, rather than through the View, is worded in src/uistrings.inc.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <bitset>
#include <windows.h>
#endif

#include "canvas.h"
#include "view.h"

struct _XDisplay; // Xlib's Display, kept out of the header (X11's macros would break what includes it)

namespace fmvibe::gui {

class Controller;

class Editor {
public:
    explicit Editor(Controller &controller);
    ~Editor();
    Editor(const Editor &) = delete;
    Editor &operator=(const Editor &) = delete;

    // Sizes are in physical pixels, as CLAP specifies them.
    void size(uint32_t &width, uint32_t &height) const;
    // The size nearest `width` x `height` at the view's current proportions (the keyboard strip may be collapsed), as
    // constrain() finds it from the editor's size now.
    void fit(uint32_t &width, uint32_t &height) const;
    // The size a host's window may take at the view's proportions (a view `viewHeight` tall): `width` with the height
    // following it, or, where the host moved the height further from the editor's size (`fromWidth` x `fromHeight`)
    // than the width, as a window dragged by its top or bottom edge (a pixel at a time too), `height` with the width
    // following it. A height of 0, a size that already keeps the proportions (but for rounding, both sides moved) or no
    // size to go from takes the width.
    static void constrain(uint32_t &width, uint32_t &height, float viewHeight, uint32_t fromWidth = 0,
                          uint32_t fromHeight = 0);
    static constexpr uint32_t kMinWidth = 600;
    static constexpr uint32_t kMaxWidth = 3600;
    // Asks the host for the size the view's proportions want, held within the monitor's work area: a state, a fold or
    // a DPI change may make the window another size.
    void fitLayout();
    bool setSize(uint32_t width, uint32_t height);
    bool show(bool visible);
    // Opens the preset file dialog, after the current mouse gesture is over.
    void choosePreset(bool save);
    // Likewise the tuning file dialog: a scale or tuning, or (`mapping`) a keyboard mapping, loaded into the patch.
    void chooseTuning(bool mapping);
    // The whole editor painted again: the layout changed under it (a state loaded), which its poll doesn't watch.
    void repaint() { invalidate(); }
    // The zoom the host asks for (CLAP's set_scale, VST3's content scale): 1 at 96 DPI. The editor keeps its zoom,
    // the host is asked for the size that has it at the new scale.
    void setScale(double scale);
    // The scale a new editor opens at, before the host has given it a window.
    static float systemScale();

#ifdef _WIN32
    bool attach(HWND parent);
    // A key the host's window took in, which the editor's keyboard hook passes on: true if it was
    // an undo or redo shortcut meant for this editor, which the host then doesn't see.
    bool shortcut(WPARAM key, bool up);
    // The View::Key the SEQ view takes for a key (Ctrl+A, the arrows without Ctrl), if any.
    static std::optional<View::Key> seqKeyOf(WPARAM key, bool ctrl);
    // A message the host's loop took off the queue, which the editor's message hook passes on: true if it was
    // a key for a value being typed here, which the host then doesn't see.
    bool typedKey(const MSG &msg);
#else
    using WindowId = unsigned long;
    bool attach(WindowId parent);
    // The host's main thread, from the timer the plugin registered with it: the display's events, the poll, a
    // dialog's answer, and what changed painted. Nothing else touches the display.
    void pump();
    // Whether an open dialog or menu takes the input: the editor then only paints, and Escape closes the dialog.
    bool modal() const;
#endif

private:
#ifdef _WIN32
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
    // before the host gave it a parent), the host is asked for the size at it, held within the screen.
    void followDpi();
    void updateCursor();
    void contextMenu(int x, int y);
    void browsePreset(bool save);
    void browseTuning(bool mapping);
    // The editor takes the keyboard while a value is typed or a dialog is open, and hands it back after.
    bool wantsKeyboard() const { return view_.typing() || view_.dialogOpen(); }
    // Whether a key the host's loop took for the window `to` is this editor's (see typedKey()).
    bool keysFor(HWND to) const;
    bool dialogKeyFor(WPARAM key) const;
    void syncFocus();
    bool key(WPARAM key, LPARAM flags);

    Controller &controller_;
    View view_;
    Resources resources_;
    ComPtr<ID2D1HwndRenderTarget> target_;
    HWND hwnd_ = nullptr;
    bool resized_ = false; // the user changed the size, which new instances then open at
    float dpiScale_ = 1.0f; // the DPI the size is worked out for, over 96: the system's, then the window's
    uint32_t askedWidth_ = 0; // the width the editor last asked for: a size the host sets at it is no new zoom
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
#else
    class Dialogs; // the zenity/kdialog jobs, answered from the poll (editor_x11.cpp)
    class Menu;    // a context menu, grabbing the pointer until it is done

    void pumpEvents();
    void handleEvent(void *event); // an XEvent
    void tick();
    void paint();
    void ensureBack();
    void drawBack(const Rect *area);
    void invalidate();
    void invalidate(const std::vector<Rect> &areas);
    float scale() const;
    Point toView(int x, int y) const;
    void updateCursor();
    void contextMenu(int rootX, int rootY, int x, int y);
    void startPresetDialog(bool save);
    void startTuningDialog(bool mapping);
    void followScale(float scale, bool fromHost);
    void takeKeyFocus();
    void passKeyToHost(const void *event); // an XKeyEvent the editor didn't take, to the host's window
    void releaseKeyFocus();
    WindowId hostFocusWindow() const;
    bool wantsKeyboard() const { return view_.typing() || view_.dialogOpen(); }
    // The keys the editor takes itself while typing or a dialog is open: true if it took it.
    bool key(const void *xkey);   // an XKeyEvent
    bool dialogKeyFor(int keysym) const;
    // A notice for a moment (`text`), where a dialog tool is missing or a file couldn't be written.
    void notify(std::string text);
    void setClipboard(std::string_view text);
    void requestClipboard(bool cut);
    void answerSelection(void *request);

    Controller &controller_;
    View view_;
    Resources resources_;
    _XDisplay *display_ = nullptr;
    WindowId window_ = 0;
    cairo_surface_t *surface_ = nullptr;   // the window's, or nullptr while it has none
    cairo_surface_t *back_ = nullptr;      // what the window shows: drawn whole, then copied in, so a frame never tears
    bool backValid_ = false;
    bool expose_ = false;                  // the window asks for what the back buffer holds: a copy, no redraw
    std::unique_ptr<Dialogs> dialogs_;
    std::unique_ptr<Menu> menu_;
    bool resized_ = false;
    float dpiScale_ = 1.0f;
    bool hostScaled_ = false; // the host gave a scale (setScale()), which the desktop's no longer overrides
    uint32_t askedWidth_ = 0; // the width the editor last asked for: a size the host sets at it is no new zoom
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool whole_ = true;             // the next paint draws every of it
    std::vector<Rect> areas_;       // what changed, in the view's coordinates
    int pendingPreset_ = -1;        // 0 load, 1 save: after the mouse gesture is over
    int pendingTuning_ = -1;        // 0 a scale, 1 a keyboard mapping: likewise
    uint64_t lastClickMs_ = 0;      // for a double-click: the last press's time and place
    int lastClickX_ = 0, lastClickY_ = 0;
    bool dragging_ = false;         // the pointer is grabbed for a drag
    bool hasKeyboard_ = false;
    WindowId keyboardFrom_ = 0;
    std::string clipboard_;
    bool clipboardAsked_ = false;   // a paste waiting for the selection's owner
    unsigned long clipboardTarget_ = 0; // the atom the paste asked for, to fall back from UTF8_STRING to STRING
    unsigned long cursors_[7] = {}; // one per CursorKind, made once and kept while the window lives
    std::string notice_;
    uint64_t noticeUntil_ = 0;
#endif
};

inline void Editor::constrain(uint32_t &width, uint32_t &height, float viewHeight, uint32_t fromWidth,
                              uint32_t fromHeight) {
    if (height > 0 && fromWidth > 0 && fromHeight > 0 && viewHeight > 0.0f) {
        const auto followed = static_cast<uint32_t>(
            std::lround(std::clamp(width, kMinWidth, kMaxWidth) * viewHeight / View::kWidth));
        // A size held within the screen floors both sides, so with the width moved too, a pixel or two off is still the
        // width's. With the width as it was, a pixel is a drag: a host offers an edge dragged slowly a pixel at a time.
        const bool rounded = width != fromWidth && (followed > height ? followed - height : height - followed) <= 2;
        const double widthMoved = std::fabs(static_cast<double>(width) - fromWidth) / fromWidth;
        const double heightMoved = std::fabs(static_cast<double>(height) - fromHeight) / fromHeight;
        if (followed != height && !rounded && heightMoved > widthMoved)
            width = static_cast<uint32_t>(std::lround(height * View::kWidth / viewHeight));
    }
    width = std::clamp(width, kMinWidth, kMaxWidth);
    height = static_cast<uint32_t>(std::lround(width * viewHeight / View::kWidth));
}

} // namespace fmvibe::gui
