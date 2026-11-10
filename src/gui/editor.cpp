#include "editor.h"

#include "../uistrings.h"
#include "../usersettings.h"

#include <windowsx.h>
#ifndef WM_DPICHANGED_AFTERPARENT
#define WM_DPICHANGED_AFTERPARENT 0x02E3 // Windows 10 1703 on; older headers don't name it
#endif

#include <commdlg.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace fmvibe::gui {

namespace {

constexpr wchar_t kClassName[] = L"FMVibeEditor";
constexpr UINT_PTR kPollTimer = 1;
constexpr UINT kPollMs = 15; // about 60 frames a second: Windows timers tick every 15.6 ms
constexpr uint32_t kMinWidth = 600;
constexpr uint32_t kMaxWidth = 3600;
constexpr UINT kPresetMessage = WM_APP + 1; // posted, so the dialog opens after the mouse gesture ends

int liveEditors = 0;

// Hosts keep the keyboard, so a hook on the editors' thread watches the keys it takes in, and hands
// the undo and redo shortcuts to an editor under the mouse; see Editor::shortcut().
std::vector<Editor *> hookedEditors;
HHOOK keyboardHook = nullptr;
// Some hosts' message loops (Bitwig's) send the keys on to the host rather than to the window that has the
// keyboard, so while a value is typed (or the preset browser, with its search, is open) a second hook takes them
// off the queue first; see Editor::typedKey().
HHOOK messageHook = nullptr;

LRESULT CALLBACK watchKeys(int code, WPARAM key, LPARAM flags) {
    if (code == HC_ACTION) { // not HC_NOREMOVE: a key peeked at comes again when it is taken
        const bool up = (static_cast<uint32_t>(flags) & 0x80000000u) != 0;
        for (Editor *editor : hookedEditors)
            if (editor->shortcut(key, up))
                return 1; // the host doesn't see it
    }
    return CallNextHookEx(keyboardHook, code, key, flags);
}

LRESULT CALLBACK takeTypedKeys(int code, WPARAM removal, LPARAM message) {
    if (code == HC_ACTION && removal == PM_REMOVE) { // a message peeked at comes again when it is taken
        MSG &msg = *reinterpret_cast<MSG *>(message);
        for (Editor *editor : hookedEditors)
            if (editor->typedKey(msg)) {
                msg.message = WM_NULL; // what the host's loop gets instead: nothing to pass on
                break;
            }
    }
    return CallNextHookEx(messageHook, code, removal, message);
}

// Shift, Ctrl, Alt and the like go on to the host even while typing, so its idea of them stays right.
bool isModifier(WPARAM key) {
    switch (key) {
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
    case VK_LWIN:
    case VK_RWIN:
    case VK_CAPITAL:
        return true;
    default:
        return false;
    }
}

HINSTANCE moduleHandle() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&moduleHandle), &module);
    return module;
}

std::wstring widen(const std::string &utf8) {
    std::wstring wide(utf8.size(), L'\0');
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(),
                                           static_cast<int>(wide.size()));
    wide.resize(std::max(length, 0));
    return wide;
}

// Text onto the clipboard, and what it holds, as UTF-8.
void putOnClipboard(HWND owner, const std::string &utf8) {
    if (utf8.empty() || !OpenClipboard(owner))
        return;
    EmptyClipboard();
    const std::wstring wide = widen(utf8);
    if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (wide.size() + 1) * sizeof(wchar_t))) {
        if (void *to = GlobalLock(memory)) {
            std::memcpy(to, wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(memory);
            if (!SetClipboardData(CF_UNICODETEXT, memory))
                GlobalFree(memory);
        } else {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
}

std::string clipboardText(HWND owner) {
    std::string utf8;
    if (!OpenClipboard(owner))
        return utf8;
    if (HANDLE memory = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto *wide = static_cast<const wchar_t *>(GlobalLock(memory))) {
            const int length = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
            if (length > 1) {
                utf8.resize(length);
                WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8.data(), length, nullptr, nullptr);
                utf8.resize(length - 1); // without the NUL
            }
            GlobalUnlock(memory);
        }
    }
    CloseClipboard();
    return utf8;
}

Modifiers modifiers() {
    return {GetKeyState(VK_SHIFT) < 0, GetKeyState(VK_CONTROL) < 0, GetKeyState(VK_MENU) < 0};
}

// A modal loop (a file dialog, a message box, a menu) lets the host go on meanwhile, and it may close the editor, or
// remove the plugin and unload this DLL, before the loop returns. While one runs, the DLL is held loaded; after it,
// the code that ran it goes on only while the editor is still there. If the editor is gone, the DLL stays loaded for
// good: letting it go there would unload the code still running.
class ModalGuard {
public:
    explicit ModalGuard(std::shared_ptr<bool> alive) : alive_(std::move(alive)) {
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&moduleHandle), &module_);
    }
    ~ModalGuard() {
        if (*alive_ && module_) // the plugin is still there, so the host still holds the DLL too
            FreeLibrary(module_);
    }
    ModalGuard(const ModalGuard &) = delete;
    ModalGuard &operator=(const ModalGuard &) = delete;
    bool alive() const { return *alive_; }

private:
    std::shared_ptr<bool> alive_;
    HMODULE module_ = nullptr;
};

} // namespace

Editor::Editor(Controller &controller) : controller_(controller), view_(controller) {
    // The class lives in this DLL, so register it while editors exist and
    // unregister with the last one; the DLL may be unloaded afterwards.
    if (liveEditors++ == 0) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = &Editor::windowProc;
        wc.hInstance = moduleHandle();
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);
    }
    if (hookedEditors.empty()) {
        keyboardHook = SetWindowsHookExW(WH_KEYBOARD, &watchKeys, nullptr, GetCurrentThreadId());
        messageHook = SetWindowsHookExW(WH_GETMESSAGE, &takeTypedKeys, nullptr, GetCurrentThreadId());
    }
    hookedEditors.push_back(this);

    // Hosts ask for the size before giving the editor a parent: at the system's DPI until it has one (see followDpi()).
    dpiScale_ = GetDpiForSystem() / 96.0f;
    width_ = static_cast<uint32_t>(std::lround(View::kWidth * controller_.layout().zoom * dpiScale_));
    height_ = 0;
    fit(width_, height_);
}

Editor::~Editor() {
    *alive_ = false;
    std::erase(hookedEditors, this);
    if (hookedEditors.empty() && keyboardHook) {
        UnhookWindowsHookEx(keyboardHook);
        keyboardHook = nullptr;
    }
    if (hookedEditors.empty() && messageHook) {
        UnhookWindowsHookEx(messageHook);
        messageHook = nullptr;
    }
    view_.endTyping(false);
    syncFocus();
    if (resized_) { // the size is the user's, so the next instance opens at it
        userSettings.zoom = controller_.layout().zoom;
        writeUserSettings();
    }
    if (hwnd_) {
        KillTimer(hwnd_, kPollTimer);
        DestroyWindow(hwnd_);
    }
    if (--liveEditors == 0)
        UnregisterClassW(kClassName, moduleHandle());
}

void Editor::size(uint32_t &width, uint32_t &height) const {
    width = width_;
    height = height_;
}

void Editor::constrain(uint32_t &width, uint32_t &height, float viewHeight) {
    width = std::clamp(width, kMinWidth, kMaxWidth);
    height = static_cast<uint32_t>(std::lround(width * viewHeight / View::kWidth));
}

void Editor::fit(uint32_t &width, uint32_t &height) const {
    constrain(width, height, view_.height());
}

bool Editor::setSize(uint32_t width, uint32_t height) {
    fit(width, height);
    width_ = width;
    height_ = height;
    // The size asked for to follow the DPI keeps the zoom, even where the size is held to its limits: moved back to
    // the monitor it came from, the editor then comes back at the size it had.
    if (width != dpiWidth_) {
        const float zoom = width / (View::kWidth * dpiScale_);
        resized_ = resized_ || std::fabs(zoom - controller_.layout().zoom) > 1e-4f;
        controller_.layout().zoom = zoom;
    }
    dpiWidth_ = 0;
    if (hwnd_)
        SetWindowPos(hwnd_, nullptr, 0, 0, static_cast<int>(width), static_cast<int>(height),
                     SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
    return true;
}

bool Editor::attach(HWND parent) {
    if (hwnd_ || !parent)
        return false;
    CreateWindowExW(0, kClassName, L"FMVibe", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0,
                    static_cast<int>(width_), static_cast<int>(height_), parent, nullptr, moduleHandle(), this);
    if (!hwnd_)
        return false;
    SetTimer(hwnd_, kPollTimer, kPollMs, nullptr);
    followDpi(); // on a monitor scaled unlike the primary one, in a host aware of it
    return true;
}

void Editor::followDpi() {
    const UINT dpi = hwnd_ ? GetDpiForWindow(hwnd_) : 0;
    if (dpi == 0)
        return;
    const float dpiScale = dpi / 96.0f;
    if (std::fabs(dpiScale - dpiScale_) < 1e-3f)
        return;
    dpiScale_ = dpiScale;
    uint32_t width = static_cast<uint32_t>(std::lround(View::kWidth * controller_.layout().zoom * dpiScale_));
    uint32_t height = 0;
    fit(width, height);
    if (width != width_ || height != height_) {
        dpiWidth_ = width;
        controller_.requestEditorSize(width, height); // the host then sets it (setSize()), at the same zoom
    }
}

bool Editor::show(bool visible) {
    if (!hwnd_)
        return false;
    ShowWindow(hwnd_, visible ? SW_SHOWNA : SW_HIDE);
    return true;
}

LRESULT CALLBACK Editor::windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto *editor = static_cast<Editor *>(reinterpret_cast<CREATESTRUCTW *>(lParam)->lpCreateParams);
        editor->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(editor));
    }
    auto *editor = reinterpret_cast<Editor *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return editor ? editor->handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

void Editor::tick() {
    lastPoll_ = GetTickCount64();
    // Ctrl let go, or pressed with the mouse over the editor: while the host has the keyboard no key message comes
    // here, and only the next mouse move would tell the view (whose grid shows while Ctrl is held). A press counts
    // only while a window of this process is in front, not Ctrl held in another application.
    const bool ctrl = GetAsyncKeyState(VK_CONTROL) < 0;
    DWORD front = 0;
    if (const HWND foreground = GetForegroundWindow())
        GetWindowThreadProcessId(foreground, &front);
    const bool ours = front == GetCurrentProcessId();
    const bool ctrlChanged = (!ctrl || (trackingLeave_ && ours)) && view_.setCtrl(ctrl);
    if (ctrlChanged)
        invalidate();
    else if (view_.poll())
        invalidate(view_.changedAreas());
}

LRESULT Editor::handle(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_PAINT:
        paint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        width_ = LOWORD(lParam);
        height_ = HIWORD(lParam);
        if (target_)
            target_->Resize(D2D1::SizeU(width_, height_));
        invalidate();
        return 0;
    case WM_TIMER:
        if (wParam == kPollTimer)
            tick();
        return 0;
    case WM_DPICHANGED_AFTERPARENT: // the host's window went to a monitor scaled otherwise
        followDpi();
        return 0;
    case kPresetMessage:
        browsePreset(wParam != 0);
        return 0;
    case WM_MOUSEMOVE: {
        if (!trackingLeave_) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd_, 0};
            trackingLeave_ = TrackMouseEvent(&track) != FALSE;
        }
        const bool changed = view_.mouseMove(toView(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)), modifiers());
        updateCursor();
        if (changed) // else nothing to paint again: the poll timer paints what animates meanwhile
            invalidate();
        // Windows sends WM_TIMER only while no other message waits, so a mouse kept moving (cards dragged along the
        // row, nodes in the graph) holds the poll timer off: the moves poll in its place, as often as it would.
        if (GetTickCount64() - lastPoll_ >= kPollMs)
            tick();
        return 0;
    }
    case WM_MOUSELEAVE:
        trackingLeave_ = false;
        view_.mouseLeave();
        invalidate();
        return 0;
    case WM_LBUTTONDOWN:
        SetCapture(hwnd_);
        view_.mouseDown(toView(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)), modifiers());
        syncFocus();
        updateCursor();
        invalidate();
        return 0;
    case WM_LBUTTONUP:
        view_.mouseUp(toView(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)), modifiers());
        if (GetCapture() == hwnd_)
            ReleaseCapture();
        updateCursor();
        invalidate();
        return 0;
    case WM_CAPTURECHANGED:
        // Capture taken away mid-drag (alt-tab, host dialog): the gesture ends where it is, but nothing is dropped
        // where the mouse happens to be. After a button-up, which let the capture go, there is no drag left.
        view_.cancelDrag();
        invalidate();
        return 0;
    case WM_LBUTTONDBLCLK:
        // Stands in for the second WM_LBUTTONDOWN, so capture like one: a key it presses
        // must still see its button-up.
        SetCapture(hwnd_);
        view_.doubleClick(toView(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)), modifiers());
        syncFocus();
        updateCursor();
        invalidate();
        return 0;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK: // a quick second press is a press too
        SetCapture(hwnd_); // the drag goes on outside the window
        view_.middleDown(toView(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)));
        syncFocus();
        updateCursor();
        invalidate();
        return 0;
    case WM_MBUTTONUP:
        view_.middleUp(toView(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)));
        if (GetCapture() == hwnd_ && !(wParam & MK_LBUTTON)) // not from under a drag of the left button
            ReleaseCapture();
        updateCursor();
        invalidate();
        return 0;
    case WM_RBUTTONUP: {
        const std::shared_ptr<bool> alive = alive_; // the menu's modal loop may outlast the editor
        contextMenu(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        if (*alive)
            syncFocus();
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd_, &p);
        view_.wheel(toView(p.x, p.y), GET_WHEEL_DELTA_WPARAM(wParam) / static_cast<float>(WHEEL_DELTA), modifiers());
        syncFocus();
        invalidate();
        return 0;
    }
    case WM_GETDLGCODE: // in a host's dialog, Enter, Escape and the arrows still come here
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
    case WM_KEYDOWN:
        if (wParam == VK_CONTROL && view_.setCtrl(true)) // a graph's grid shows while Ctrl is held
            invalidate();
        if (key(wParam, lParam)) {
            syncFocus();
            invalidate();
            return 0;
        }
        break;
    case WM_KEYUP:
        if (wParam == VK_CONTROL && view_.setCtrl(false))
            invalidate();
        break;
    case WM_CHAR:
        if (view_.typing())
            return 0; // key() took the character already: not every host translates keys into WM_CHAR
        break;
    case WM_KILLFOCUS:
        if (view_.setCtrl(false)) // no key-up comes once the keyboard is gone
            invalidate();
        // Clicked away into the host: the value typed so far counts.
        if (view_.typing()) {
            hasKeyboard_ = false;
            keyboardFrom_ = nullptr;
            view_.endTyping(true);
            invalidate();
        }
        break;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            updateCursor();
            return TRUE;
        }
        break;
    case WM_DESTROY:
        KillTimer(hwnd_, kPollTimer);
        target_.Reset();
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        hwnd_ = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

float Editor::scale() const {
    return std::min(width_ / View::kWidth, height_ / view_.height());
}

Point Editor::toView(int x, int y) const {
    const float s = scale();
    const float offsetX = (width_ - View::kWidth * s) * 0.5f;
    const float offsetY = (height_ - view_.height() * s) * 0.5f;
    return {(x - offsetX) / s, (y - offsetY) / s};
}

void Editor::invalidate() {
    if (hwnd_)
        InvalidateRect(hwnd_, nullptr, FALSE);
}

void Editor::invalidate(const std::vector<Rect> &areas) {
    if (!hwnd_)
        return;
    const float s = scale();
    const float offsetX = (width_ - View::kWidth * s) * 0.5f;
    const float offsetY = (height_ - view_.height() * s) * 0.5f;
    for (const Rect &a : areas) { // in whole pixels, out from the area's edges
        const RECT r{static_cast<LONG>(std::floor(a.x * s + offsetX)), static_cast<LONG>(std::floor(a.y * s + offsetY)),
                     static_cast<LONG>(std::ceil(a.right() * s + offsetX)),
                     static_cast<LONG>(std::ceil(a.bottom() * s + offsetY))};
        InvalidateRect(hwnd_, &r, FALSE);
    }
}

void Editor::paint() {
    // What is to be painted: a few rects of the window (what View::changedAreas() moved), or the whole of it.
    std::vector<RECT> parts;
    if (const HRGN region = CreateRectRgn(0, 0, 0, 0)) {
        if (const int kind = GetUpdateRgn(hwnd_, region, FALSE); kind == SIMPLEREGION || kind == COMPLEXREGION) {
            const DWORD bytes = GetRegionData(region, 0, nullptr);
            std::vector<char> buffer(bytes);
            auto *data = reinterpret_cast<RGNDATA *>(buffer.data());
            if (bytes >= sizeof(RGNDATAHEADER) && GetRegionData(region, bytes, data) == bytes) {
                const auto *rects = reinterpret_cast<const RECT *>(data->Buffer);
                parts.assign(rects, rects + data->rdh.nCount);
            }
        }
        DeleteObject(region);
    }
    PAINTSTRUCT ps;
    BeginPaint(hwnd_, &ps);
    bool lost = false;
    bool fresh = false; // a new target holds nothing of the frame before
    if (!target_) {
        const D2D1_RENDER_TARGET_PROPERTIES props =
            D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), 96.0f, 96.0f);
        // It keeps each frame once shown, so a frame can repaint only part of it.
        resources_.factory()->CreateHwndRenderTarget(
            props,
            D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(width_, height_), D2D1_PRESENT_OPTIONS_RETAIN_CONTENTS),
            target_.GetAddressOf());
        fresh = true;
    }
    if (target_) {
        const float s = scale();
        const float offsetX = (width_ - View::kWidth * s) * 0.5f, offsetY = (height_ - view_.height() * s) * 0.5f;
        // In the view's coordinates; the whole window when there are many parts, or one covers most of it.
        std::vector<Rect> areas;
        float covered = 0.0f;
        for (const RECT &r : parts) {
            areas.push_back({(r.left - offsetX) / s, (r.top - offsetY) / s, (r.right - r.left) / s,
                             (r.bottom - r.top) / s});
            covered += static_cast<float>((r.right - r.left) * (r.bottom - r.top));
        }
        const bool whole = fresh || parts.empty() || parts.size() > 8 ||
                           covered > 0.75f * static_cast<float>(width_) * static_cast<float>(height_);
        // The poll timer only fires once no input is waiting, so while the mouse moves, frames come from mouse moves
        // alone: give them fresh telemetry. A part takes the telemetry the poll found it changed by, so what lies
        // outside it, painted with the telemetry before, is still what the next poll compares with.
        if (whole)
            view_.refreshTelemetry();
        target_->BeginDraw();
        target_->SetTransform(D2D1::Matrix3x2F::Scale(s, s) * D2D1::Matrix3x2F::Translation(offsetX, offsetY));
        {
            Canvas canvas(target_.Get(), resources_);
            if (whole)
                view_.paint(canvas);
            else
                for (const Rect &area : areas)
                    view_.paint(canvas, area);
        }
        if (target_->EndDraw() == D2DERR_RECREATE_TARGET) {
            target_.Reset(); // device lost; recreated on the next paint, asked for below
            lost = true;
        }
    }
    EndPaint(hwnd_, &ps);
    // EndPaint takes the window as painted, though nothing showed: without another paint, a patch that stands
    // still would leave the editor blank.
    if (lost)
        invalidate();
}

void Editor::updateCursor() {
    LPCWSTR id = IDC_ARROW;
    switch (view_.cursor()) {
    case CursorKind::Arrow:
        id = IDC_ARROW;
        break;
    case CursorKind::Hand:
        id = IDC_HAND;
        break;
    case CursorKind::Vertical:
        id = IDC_SIZENS;
        break;
    case CursorKind::Horizontal:
        id = IDC_SIZEWE;
        break;
    case CursorKind::Move:
        id = IDC_SIZEALL;
        break;
    case CursorKind::Text:
        id = IDC_IBEAM;
        break;
    }
    SetCursor(LoadCursorW(nullptr, id));
}

// Keys come here while a value is typed or the preset browser is open (from the message hook), or when a host
// passes them on.
bool Editor::key(WPARAM key, LPARAM flags) {
    const bool ctrl = GetKeyState(VK_CONTROL) < 0;
    const bool shift = GetKeyState(VK_SHIFT) < 0;
    if (view_.typing()) {
        if (ctrl && key == 'A') {
            view_.typeKey(View::Key::SelectAll);
            return true;
        }
        if (ctrl && (key == 'C' || key == 'X')) {
            putOnClipboard(hwnd_, view_.copyText(key == 'X'));
            return true;
        }
        if (ctrl && key == 'V') {
            view_.pasteText(clipboardText(hwnd_));
            return true;
        }
        switch (key) {
        case VK_RETURN:
            view_.typeKey(View::Key::Enter);
            return true;
        case VK_ESCAPE:
            view_.typeKey(View::Key::Escape);
            return true;
        case VK_BACK:
            view_.typeKey(View::Key::Backspace);
            return true;
        case VK_DELETE:
            view_.typeKey(View::Key::Delete);
            return true;
        case VK_LEFT:
            view_.typeKey(View::Key::Left, shift);
            return true;
        case VK_RIGHT:
            view_.typeKey(View::Key::Right, shift);
            return true;
        case VK_HOME:
            view_.typeKey(View::Key::Home, shift);
            return true;
        case VK_END:
            view_.typeKey(View::Key::End, shift);
            return true;
        case VK_UP:
            view_.typeKey(View::Key::Up);
            return true;
        case VK_DOWN:
            view_.typeKey(View::Key::Down);
            return true;
        default: {
            // The character the key makes with the current layout and modifiers, if any. Flag 4
            // leaves the keyboard state alone, so dead keys still work for the host afterwards.
            BYTE state[256];
            wchar_t text[4];
            if (GetKeyboardState(state) &&
                ToUnicode(static_cast<UINT>(key), (static_cast<UINT>(flags) >> 16) & 0xFF, state, text, 4, 4) == 1)
                view_.typeChar(static_cast<char32_t>(text[0]));
            return true; // nothing else reaches the host while typing
        }
        }
    }
    if (ctrl && (key == 'Y' || (key == 'Z' && GetKeyState(VK_SHIFT) < 0))) {
        view_.redo();
        return true;
    }
    if (ctrl && key == 'Z') {
        view_.undo();
        return true;
    }
    return false;
}

// Ctrl+Z undoes, Ctrl+Y or Ctrl+Shift+Z redoes, while the mouse is over the editor in the active
// window, so the host's own shortcuts work everywhere else. A shortcut's key going up is taken too.
bool Editor::shortcut(WPARAM key, bool up) {
    if (key != 'Z' && key != 'Y')
        return false;
    if (up) {
        const bool taken = shortcutKey_ == key;
        if (taken)
            shortcutKey_ = 0;
        return taken;
    }
    if (!hwnd_ || view_.typing() || GetKeyState(VK_CONTROL) >= 0 || GetKeyState(VK_MENU) < 0 ||
        !IsWindowVisible(hwnd_) || GetForegroundWindow() != GetAncestor(hwnd_, GA_ROOT))
        return false;
    POINT p;
    GetCursorPos(&p);
    const HWND under = WindowFromPoint(p);
    if (under != hwnd_ && !IsChild(hwnd_, under))
        return false;
    if (key == 'Y' || GetKeyState(VK_SHIFT) < 0)
        view_.redo();
    else
        view_.undo();
    shortcutKey_ = key;
    invalidate();
    return true;
}

// While a value is typed, a key for the window around the editor comes here straight off the queue, as
// WM_KEYDOWN would bring it, and the host's loop never sees it; nor the key going up.
bool Editor::typedKey(const MSG &msg) {
    if (!hwnd_ || !msg.hwnd || (msg.hwnd != hwnd_ && GetAncestor(msg.hwnd, GA_ROOT) != GetAncestor(hwnd_, GA_ROOT)))
        return false;
    const auto code = static_cast<uint8_t>(msg.wParam);
    switch (msg.message) {
    case WM_KEYDOWN:
        if (!view_.typing() || isModifier(msg.wParam))
            return false;
        typedKeys_.set(code);
        key(msg.wParam, msg.lParam);
        syncFocus();
        invalidate();
        return true;
    case WM_KEYUP:
        if (!typedKeys_.test(code))
            return false;
        typedKeys_.reset(code);
        return true;
    case WM_CHAR:
        return view_.typing(); // key() took the character already
    default:
        return false;
    }
}

void Editor::syncFocus() {
    if (!hwnd_)
        return;
    if (view_.typing() && !hasKeyboard_) {
        keyboardFrom_ = SetFocus(hwnd_);
        hasKeyboard_ = GetFocus() == hwnd_;
    } else if (!view_.typing() && hasKeyboard_) {
        hasKeyboard_ = false;
        // Back to whoever had it, or else to the host's window around the editor.
        HWND to = keyboardFrom_ && IsWindow(keyboardFrom_) ? keyboardFrom_ : GetAncestor(hwnd_, GA_ROOT);
        keyboardFrom_ = nullptr;
        if (to && to != hwnd_)
            SetFocus(to);
    }
}

void Editor::choosePreset(bool save) {
    if (hwnd_)
        PostMessageW(hwnd_, kPresetMessage, save ? 1 : 0, 0);
}

void Editor::browsePreset(bool save) {
    const ModalGuard guard(alive_); // the dialogs below run modal loops
    // Filter pairs are NUL-separated, and the list ends with a double NUL (the last one the string's own).
    const std::wstring filter = widen(tr(Str::DialogPresetFiles)) + L'\0' + L"*.fmv" + L'\0' +
                                widen(tr(Str::DialogAllFiles)) + L'\0' + L"*.*" + L'\0';
    // Both start in the preset folder (the scan makes it if need be); saving suggests the preset's name.
    controller_.rescanPresets();
    const std::wstring folder = controller_.presetFolder();
    wchar_t path[MAX_PATH] = L"";
    if (save && controller_.presetIndex() >= 0) {
        const std::wstring name = widen(controller_.presetName());
        wcsncpy_s(path, name.c_str(), _TRUNCATE);
    }
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFilter = filter.c_str();
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrInitialDir = folder.empty() ? nullptr : folder.c_str();
    dialog.lpstrDefExt = L"fmv";
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    const std::wstring title = widen(tr(save ? Str::DialogSavePreset : Str::DialogLoadPreset));
    if (save) {
        dialog.lpstrTitle = title.c_str();
        dialog.Flags |= OFN_OVERWRITEPROMPT;
        if (!GetSaveFileNameW(&dialog) || !guard.alive())
            return;
        if (!controller_.savePreset(path))
            MessageBoxW(hwnd_, widen(tr(Str::DialogWriteFailed)).c_str(), L"FMVibe", MB_ICONWARNING);
    } else {
        dialog.lpstrTitle = title.c_str();
        dialog.Flags |= OFN_FILEMUSTEXIST;
        if (!GetOpenFileNameW(&dialog) || !guard.alive())
            return;
        if (!controller_.loadPreset(path))
            MessageBoxW(hwnd_, widen(tr(Str::DialogNotPreset)).c_str(), L"FMVibe", MB_ICONWARNING);
    }
    if (guard.alive())
        invalidate();
}

void Editor::contextMenu(int x, int y) {
    const std::vector<MenuItem> items = view_.contextMenu(toView(x, y));
    if (items.empty())
        return;
    HMENU menu = CreatePopupMenu();
    for (const MenuItem &item : items) {
        if (item.command == 0)
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        else
            AppendMenuW(menu, MF_STRING | (item.checked ? MF_CHECKED : MF_UNCHECKED),
                        static_cast<UINT_PTR>(item.command), widen(item.label).c_str());
    }
    POINT screen{x, y};
    ClientToScreen(hwnd_, &screen);
    const ModalGuard guard(alive_); // the menu runs a modal loop
    const int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, screen.x, screen.y, 0,
                                       hwnd_, nullptr);
    DestroyMenu(menu);
    if (!guard.alive())
        return;
    if (command > 0)
        view_.menuCommand(command);
    invalidate();
}

} // namespace fmvibe::gui
