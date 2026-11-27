// The editor on Linux: an X11 child window of the host's, drawn through Cairo (the Canvas of canvas_cairo.cpp),
// with the desktop's dialogs (zenity, else kdialog, run beside the host's UI thread and never waiting on it) and a
// popup menu of our own. The panel itself, and what it does, is the View, as on Windows (editor.cpp).

#include "editor.h"

#include "../presets.h"
#include "theme.h"
#include "../uistrings.h"
#include "../usersettings.h"

// Xlib's macros (Bool, None, Status, Above...) would break the headers above: it comes last.
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <cairo-xlib.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <deque>
#include <fcntl.h>
#include <functional>
#include <spawn.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char **environ;

namespace fmvibe::gui {

using namespace detail;

namespace {

uint64_t nowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// The family the Canvas paints in (canvas_cairo.cpp): FMVIBE_FONT, else Inter, else what fontconfig picks.
const char *uiFont() {
    static const std::string family = [] {
        const char *chosen = std::getenv("FMVIBE_FONT");
        return std::string(chosen && *chosen ? chosen : "Inter");
    }();
    return family.c_str();
}

// Whether a program the desktop offers (zenity, kdialog) is on the PATH.
bool onPath(const char *tool) {
    const char *path = std::getenv("PATH");
    const std::string dirs = path ? path : "/usr/bin:/bin";
    for (size_t start = 0; start <= dirs.size();) {
        const size_t end = dirs.find(':', start);
        const std::string dir = dirs.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!dir.empty() && ::access((dir + "/" + tool).c_str(), X_OK) == 0)
            return true;
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return false;
}

constexpr int kScreenMargin = 16; // room around the editor for the window frame

struct ScreenSize {
    uint32_t width = 0, height = 0;
    float scale = 1.0f; // the desktop's (see desktopScale())
};

float desktopScale(Display *display);

// The screen's size and the desktop's scale, or nothing and 1 when there is no display to ask.
ScreenSize screenSize() {
    static const ScreenSize size = [] {
        ScreenSize found;
        if (Display *display = XOpenDisplay(nullptr)) {
            found.width = static_cast<uint32_t>(DisplayWidth(display, DefaultScreen(display)));
            found.height = static_cast<uint32_t>(DisplayHeight(display, DefaultScreen(display)));
            found.scale = desktopScale(display);
            XCloseDisplay(display);
        }
        return found;
    }();
    return size;
}

// A size scaled down, as far as it takes, to sit within the screen, its proportions kept.
void keepOnScreen(uint32_t &width, uint32_t &height) {
    const ScreenSize screen = screenSize();
    const int availableWidth = static_cast<int>(screen.width) - 2 * kScreenMargin;
    const int availableHeight = static_cast<int>(screen.height) - 2 * kScreenMargin;
    if (width == 0 || height == 0 || availableWidth <= 0 || availableHeight <= 0)
        return;
    const float scale = std::min({1.0f, static_cast<float>(availableWidth) / static_cast<float>(width),
                                  static_cast<float>(availableHeight) / static_cast<float>(height)});
    if (scale >= 1.0f)
        return;
    width = static_cast<uint32_t>(std::max(1.0f, std::floor(static_cast<float>(width) * scale)));
    height = static_cast<uint32_t>(std::max(1.0f, std::floor(static_cast<float>(height) * scale)));
}

// A program started with its output on a pipe: the dialogs, whose answer is read as they end. It runs beside the
// host's UI thread: nothing waits on it, a poll of the editor's timer asks whether it is done.
class Child {
public:
    Child() = default;
    Child(const Child &) = delete;
    Child &operator=(const Child &) = delete;
    ~Child() { stop(); }

    bool start(const std::vector<std::string> &argv) {
        if (argv.empty())
            return false;
        int fds[2];
        if (::pipe2(fds, O_CLOEXEC) != 0)
            return false;
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        std::vector<char *> args;
        for (const std::string &arg : argv)
            args.push_back(const_cast<char *>(arg.c_str()));
        args.push_back(nullptr);
        pid_ = -1;
        const int spawned = posix_spawnp(&pid_, args[0], &actions, nullptr, args.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        ::close(fds[1]);
        if (spawned != 0) {
            ::close(fds[0]);
            pid_ = -1;
            return false;
        }
        fd_ = fds[0];
        ::fcntl(fd_, F_SETFL, ::fcntl(fd_, F_GETFL) | O_NONBLOCK);
        return true;
    }

    // True once it has ended: status() (its exit code, -1 for a crash) and output() are then the answer.
    bool poll() {
        if (pid_ <= 0)
            return true;
        char buffer[4096];
        for (;;) {
            if (fd_ < 0)
                break;
            const ssize_t got = ::read(fd_, buffer, sizeof buffer);
            if (got > 0) {
                output_.append(buffer, static_cast<size_t>(got));
                continue;
            }
            if (got < 0 && errno == EINTR)
                continue;
            if (got < 0 && errno == EAGAIN)
                return false;
            ::close(fd_); // its output is closed: it is ending
            fd_ = -1;
        }
        int status = 0;
        const pid_t waited = ::waitpid(pid_, &status, WNOHANG);
        if (waited == 0)
            return false;
        // waited < 0: a host that reaps its children itself (SIGCHLD ignored). The output tells whether it said yes.
        status_ = waited < 0 ? (output_.empty() ? 1 : 0) : WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        forget();
        return true;
    }

    int status() const { return status_; }
    const std::string &output() const { return output_; }

    void stop() {
        if (pid_ <= 0)
            return;
        if (::waitpid(pid_, nullptr, WNOHANG) == 0) // it hasn't ended yet: it goes with its window
            ::kill(pid_, SIGTERM);
        for (int attempt = 0; attempt < 10; ++attempt) { // dialogs quit at once on SIGTERM; the wait stays bounded
            const pid_t waited = ::waitpid(pid_, nullptr, WNOHANG);
            if (waited != 0)
                break;
            ::usleep(10000);
        }
        if (::waitpid(pid_, nullptr, WNOHANG) == 0) {
            ::kill(pid_, SIGKILL);
            for (int attempt = 0; attempt < 5; ++attempt) { // SIGKILL lands at once; never a blocking wait here
                if (::waitpid(pid_, nullptr, WNOHANG) != 0)
                    break;
                ::usleep(10000);
            }
        }
        forget();
    }

private:
    void forget() {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = -1;
        pid_ = -1;
    }

    pid_t pid_ = -1;
    int fd_ = -1;
    int status_ = -1;
    std::string output_;
};

std::string trimmed(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
        text.pop_back();
    return text;
}

// A file's extension given where the chosen name has none, as a save dialog's does.
std::string withExtension(std::string path, const char *extension) {
    const size_t slash = path.find_last_of('/'), dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        path += extension;
    return path;
}

// The X keysym the editor acts on, if it is one of the View's keys while a value is typed or a dialog is open.
std::optional<View::Key> typedKeyOf(KeySym symbol, bool shift) {
    switch (symbol) {
    case XK_Return:
    case XK_KP_Enter:
        return View::Key::Enter;
    case XK_Escape:
        return View::Key::Escape;
    case XK_BackSpace:
        return View::Key::Backspace;
    case XK_Delete:
    case XK_KP_Delete:
        return View::Key::Delete;
    case XK_Left:
    case XK_KP_Left:
        return View::Key::Left;
    case XK_Right:
    case XK_KP_Right:
        return View::Key::Right;
    case XK_Home:
    case XK_KP_Home:
        return View::Key::Home;
    case XK_End:
    case XK_KP_End:
        return View::Key::End;
    case XK_Up:
    case XK_KP_Up:
        return View::Key::Up;
    case XK_Down:
    case XK_KP_Down:
        return View::Key::Down;
    case XK_Tab:
    case XK_ISO_Left_Tab:
        return shift ? View::Key::BackTab : View::Key::Tab;
    default:
        return std::nullopt;
    }
}

// The key the SEQ view takes (see View::seqKey()): Ctrl+A, C, X, V, D and the arrows, Delete.
std::optional<View::Key> seqKeyOf(KeySym symbol, bool ctrl) {
    if (ctrl)
        return symbol == XK_a   ? std::optional(View::Key::SelectAll)
               : symbol == XK_c ? std::optional(View::Key::Copy)
               : symbol == XK_x ? std::optional(View::Key::Cut)
               : symbol == XK_v ? std::optional(View::Key::Paste)
               : symbol == XK_d ? std::optional(View::Key::Duplicate)
                                : std::nullopt;
    switch (symbol) {
    case XK_Left:
        return View::Key::Left;
    case XK_Right:
        return View::Key::Right;
    case XK_Up:
        return View::Key::Up;
    case XK_Down:
        return View::Key::Down;
    case XK_Delete:
        return View::Key::Delete;
    default:
        return std::nullopt;
    }
}

Modifiers modifiersOf(unsigned state) {
    return {(state & ShiftMask) != 0, (state & ControlMask) != 0, (state & Mod1Mask) != 0};
}

// The scale the desktop's Xft.dpi asks for, over 96: what GTK and Qt windows open at on a scaled X screen.
float desktopScale(Display *display) {
    float scale = 1.0f;
    Atom actual = None;
    int format = 0;
    unsigned long count = 0, after = 0;
    unsigned char *value = nullptr;
    if (XGetWindowProperty(display, DefaultRootWindow(display), XInternAtom(display, "RESOURCE_MANAGER", False), 0,
                           8192, False, XA_STRING, &actual, &format, &count, &after, &value) == Success &&
        value) {
        if (const char *dpi = std::strstr(reinterpret_cast<const char *>(value), "Xft.dpi:")) {
            const double asked = std::atof(dpi + 8);
            if (asked >= 48.0 && asked <= 480.0)
                scale = static_cast<float>(asked / 96.0);
        }
        XFree(value);
    }
    return scale;
}

// Xlib's default handler ends the process on a stale id (a window the host destroyed meanwhile); none of ours is worth
// that. The handler is the process's, the host's too: errors on our displays are ignored, any other goes to the handler
// the host had, and that one is put back when the last editor closes (unless something else took the place meanwhile).
std::vector<Display *> ourDisplays_;
int (*hostXErrorHandler_)(Display *, XErrorEvent *) = nullptr;

int ourXError(Display *display, XErrorEvent *error) {
    for (Display *ours : ourDisplays_)
        if (ours == display)
            return 0;
    return hostXErrorHandler_ ? hostXErrorHandler_(display, error) : 0;
}

constexpr long kWindowEvents = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                               LeaveWindowMask | StructureNotifyMask | FocusChangeMask | KeyPressMask |
                               KeyReleaseMask | PropertyChangeMask;

} // namespace

// --- the desktop's dialogs -----------------------------------------------------------------------------------------
// Never modal: each call returns at once and its `then` runs from poll(), once the dialog is closed, on the host's UI
// thread. The host's loop keeps running meanwhile (a loop waiting here would make the desktop call the host "not
// responding"). One dialog at a time: the next waits in a queue.

class Editor::Dialogs {
public:
    enum class Tool { Missing, Zenity, KDialog };

    // Which tool the desktop offers, asked as a dialog opens: one installed since the last is taken up at once.
    static Tool which() { return onPath("zenity") ? Tool::Zenity : onPath("kdialog") ? Tool::KDialog : Tool::Missing; }

    using Then = std::function<void(const std::string &)>;

    void message(const std::string &title, const std::string &text, bool warning, bool yesNo,
                 std::function<void(bool)> then = {}) {
        auto answer = [then](int status, const std::string &) {
            if (then)
                then(status == 0);
        };
        switch (which()) {
        case Tool::Zenity:
            start({"zenity", yesNo ? "--question" : warning ? "--warning" : "--info", "--title=" + title,
                   "--text=" + text, "--no-markup", "--width=380"},
                  answer);
            break;
        case Tool::KDialog:
            start({"kdialog", "--title", title, yesNo ? "--warningyesno" : warning ? "--sorry" : "--msgbox", text},
                  answer);
            break;
        default:
            if (fallback)
                fallback(text);
            if (then)
                then(false);
            break;
        }
    }

    // A preset's file, or a DX7 sysex file to import (see PluginCore::importDx7()).
    void openFile(const std::string &title, const std::string &folder, bool sysex, Then then) {
        auto answer = pathThen(std::move(then));
        if (which() == Tool::Zenity)
            start({"zenity", "--file-selection", "--title=" + title,
                   "--file-filter=" + (sysex ? dx7Filter() : presetFilter()), "--file-filter=All files | *",
                   "--filename=" + startFolder(folder)},
                  answer);
        else if (which() == Tool::KDialog)
            start({"kdialog", "--title", title, "--getopenfilename", startFolder(folder),
                   sysex ? "*.syx|" + dx7Filter() : "*.fmv|" + presetFilter()},
                  answer);
        else
            noTool();
    }

    void saveFile(const std::string &title, const std::string &folder, const std::string &name, Then then) {
        auto answer = pathThen([then](const std::string &chosen) {
            if (then)
                then(withExtension(chosen, ".fmv"));
        });
        const std::string suggestion = startFolder(folder) + name;
        if (which() == Tool::Zenity)
            start({"zenity", "--file-selection", "--save", "--confirm-overwrite", "--title=" + title,
                   "--file-filter=" + presetFilter(), "--file-filter=All files | *", "--filename=" + suggestion},
                  answer);
        else if (which() == Tool::KDialog)
            start({"kdialog", "--title", title, "--getsavefilename", suggestion, "*.fmv|" + presetFilter()}, answer);
        else
            noTool();
    }

    bool busy() const { return child_ || !queue_.empty(); }

    // The editor's timer: a closed dialog answers, the next one opens.
    void poll() {
        if (!child_ || !child_->poll())
            return;
        const int status = child_->status();
        const std::string output = child_->output();
        Done done = std::move(done_);
        done_ = nullptr;
        child_.reset();
        if (done)
            done(status, output);
        next();
    }

    // Escape: the open dialog closes as cancelled, and the next opens.
    void close() {
        if (!child_)
            return;
        Done done = std::move(done_);
        done_ = nullptr;
        child_.reset(); // stops it
        if (done)
            done(1, {});
        next();
    }

    // The editor closes: the dialogs with it, no `then` run.
    void cancel() {
        queue_.clear();
        done_ = nullptr;
        child_.reset();
    }

    std::function<void(const std::string &)> fallback; // no dialog tool: the editor says what's missing

private:
    using Done = std::function<void(int status, const std::string &out)>;

    struct Job {
        std::vector<std::string> argv;
        Done done;
    };

    // zenity's filter is its name, a bar and its patterns; the wording alone is the name, the patterns come after.
    static std::string presetFilter() { return std::string(tr(Str::DialogPresetFiles)) + " | *.fmv"; }
    static std::string dx7Filter() { return std::string(tr(Str::DialogDx7Files)) + " | *.syx"; }

    static std::string startFolder(const std::string &folder) {
        if (folder.empty())
            return std::string(getenv("HOME") ? getenv("HOME") : "/") + "/";
        return folder + "/";
    }

    void start(std::vector<std::string> argv, Done done) {
        queue_.push_back({std::move(argv), std::move(done)});
        next();
    }

    void next() {
        while (!child_ && !queue_.empty()) {
            Job job = std::move(queue_.front());
            queue_.pop_front();
            auto child = std::make_unique<Child>();
            if (child->start(job.argv)) {
                child_ = std::move(child);
                done_ = std::move(job.done);
            } else if (job.done) {
                job.done(-1, {});
            }
        }
    }

    static Done pathThen(Then then) {
        return [then](int status, const std::string &out) {
            const std::string path = trimmed(out);
            if (status == 0 && !path.empty() && then)
                then(path);
        };
    }

    // No zenity, no kdialog: the panel says so a moment (oxo synth2's fallback), and nothing opens.
    void noTool() {
        if (fallback)
            fallback(tr(Str::LinuxNoDialogTool));
    }

    std::deque<Job> queue_;
    std::unique_ptr<Child> child_;
    Done done_;
};

// --- the context menu ---------------------------------------------------------------------------------------------
// An override-redirect window that grabs the pointer and the keyboard, fed by the editor's event pump until it is
// done (X has no TrackPopupMenu: a loop of our own would hold the host's UI thread).

class Editor::Menu {
public:
    Menu(Display *display, std::vector<MenuItem> items, int rootX, int rootY)
        : display_(display), items_(std::move(items)), opened_(nowMs()) {
        constexpr int kRow = 22, kSeparation = 9, kPadX = 12, kCheck = 22, kPad = 4;
        int width = 80;
        for (const MenuItem &item : items_)
            if (item.command != 0)
                width = std::max(width, static_cast<int>(kCheck + textWidth(item.label) + kPadX * 2));
        w_ = width;
        h_ = kPad * 2;
        for (const MenuItem &item : items_) {
            const int height = item.command == 0 ? kSeparation : kRow;
            rows_.push_back({0.0f, static_cast<float>(h_), static_cast<float>(w_), static_cast<float>(height)});
            h_ += height;
        }
        const int screenW = DisplayWidth(display_, DefaultScreen(display_));
        const int screenH = DisplayHeight(display_, DefaultScreen(display_));
        if (rootX + w_ > screenW)
            rootX = std::max(0, rootX - w_);
        if (rootY + h_ > screenH)
            rootY = std::max(0, screenH - h_);
        x_ = rootX;
        y_ = rootY;
        XSetWindowAttributes attributes{};
        attributes.override_redirect = True;
        attributes.save_under = True;
        attributes.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | KeyPressMask;
        win_ = XCreateWindow(display_, DefaultRootWindow(display_), x_, y_, static_cast<unsigned>(w_),
                             static_cast<unsigned>(h_), 0, CopyFromParent, InputOutput, CopyFromParent,
                             CWOverrideRedirect | CWSaveUnder | CWEventMask, &attributes);
        const Atom type = XInternAtom(display_, "_NET_WM_WINDOW_TYPE", False);
        const Atom popup = XInternAtom(display_, "_NET_WM_WINDOW_TYPE_POPUP_MENU", False);
        XChangeProperty(display_, win_, type, XA_ATOM, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char *>(&popup), 1);
        XMapRaised(display_, win_);
        surface_ = cairo_xlib_surface_create(display_, win_, DefaultVisual(display_, DefaultScreen(display_)), w_, h_);
        paint();
        // Without the grabs no event would reach the menu and nothing could close it: it goes at once instead.
        if (XGrabPointer(display_, win_, False, ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync,
                         GrabModeAsync, None, None, CurrentTime) != GrabSuccess ||
            XGrabKeyboard(display_, win_, False, GrabModeAsync, GrabModeAsync, CurrentTime) != GrabSuccess) {
            XUngrabPointer(display_, CurrentTime);
            done_ = true;
        }
        XFlush(display_);
    }

    ~Menu() {
        XUngrabKeyboard(display_, CurrentTime);
        XUngrabPointer(display_, CurrentTime);
        if (surface_)
            cairo_surface_destroy(surface_);
        if (win_)
            XDestroyWindow(display_, win_);
        XFlush(display_);
    }

    Menu(const Menu &) = delete;
    Menu &operator=(const Menu &) = delete;

    bool done() const { return done_; }
    int result() const { return result_; } // the chosen item's command, 0 for none

    // An event of the display while the menu is open: false when it isn't the menu's.
    bool handle(XEvent &event) {
        if (done_)
            return false;
        if (event.xany.window != win_)
            return false;
        switch (event.type) {
        case Expose:
            paint();
            break;
        case MotionNotify: {
            const int hot = itemAt(event.xmotion.x, event.xmotion.y);
            if (hot != hot_) {
                hot_ = hot;
                paint();
            }
            break;
        }
        case ButtonPress:
            if (event.xbutton.button <= 3) {
                const int item = itemAt(event.xbutton.x, event.xbutton.y);
                if (item >= 0)
                    pressedInside_ = true;
                else
                    done_ = true; // outside every item: cancelled
            }
            break;
        case ButtonRelease:
            if (event.xbutton.button <= 3) {
                const int item = itemAt(event.xbutton.x, event.xbutton.y);
                // The release of the click that opened the menu chooses only after a drag onto an item, as a popup menu.
                if (item >= 0 && (pressedInside_ || nowMs() - opened_ > 350)) {
                    result_ = items_[static_cast<size_t>(item)].command;
                    done_ = true;
                }
            }
            break;
        case KeyPress: {
            const KeySym symbol = XLookupKeysym(&event.xkey, 0);
            if (symbol == XK_Escape) {
                done_ = true;
            } else if (symbol == XK_Down || symbol == XK_Up) {
                const int step = symbol == XK_Down ? 1 : -1;
                int at = hot_;
                for (size_t n = 0; n < items_.size(); ++n) {
                    at = (at + step + static_cast<int>(items_.size())) % static_cast<int>(items_.size());
                    if (items_[static_cast<size_t>(at)].command != 0)
                        break;
                }
                hot_ = at;
                paint();
            } else if ((symbol == XK_Return || symbol == XK_KP_Enter) && hot_ >= 0 &&
                       items_[static_cast<size_t>(hot_)].command != 0) {
                result_ = items_[static_cast<size_t>(hot_)].command;
                done_ = true;
            }
            break;
        }
        default:
            break;
        }
        return true;
    }

private:
    double textWidth(const std::string &text) const {
        cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
        cairo_t *cr = cairo_create(surface);
        cairo_select_font_face(cr, uiFont(), CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 12.5);
        cairo_text_extents_t extents{};
        cairo_text_extents(cr, text.c_str(), &extents);
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        return extents.x_advance;
    }

    int itemAt(int x, int y) const {
        for (size_t i = 0; i < rows_.size(); ++i) {
            const Rect &r = rows_[i];
            if (x >= r.x && x < r.right() && y >= r.y && y < r.bottom() && items_[i].command != 0)
                return static_cast<int>(i);
        }
        return -1;
    }

    void paint() {
        cairo_t *cr = cairo_create(surface_);
        cairo_set_source_rgb(cr, 0.13, 0.14, 0.16);
        cairo_paint(cr);
        cairo_set_source_rgb(cr, 0.30, 0.31, 0.34);
        cairo_set_line_width(cr, 1.0);
        cairo_rectangle(cr, 0.5, 0.5, w_ - 1.0, h_ - 1.0);
        cairo_stroke(cr);
        cairo_select_font_face(cr, uiFont(), CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 12.5);
        cairo_font_extents_t extents{};
        cairo_font_extents(cr, &extents);
        for (size_t i = 0; i < items_.size(); ++i) {
            const MenuItem &item = items_[i];
            const Rect row = rows_[i];
            const float middle = row.y + row.h * 0.5f;
            if (item.command == 0) {
                cairo_set_source_rgb(cr, 0.28, 0.29, 0.32);
                cairo_rectangle(cr, 8.0, middle, w_ - 16.0, 1.0);
                cairo_fill(cr);
                continue;
            }
            if (static_cast<int>(i) == hot_) {
                cairo_set_source_rgb(cr, 0.23, 0.31, 0.42);
                cairo_rectangle(cr, 3.0, row.y + 1.0, w_ - 6.0, row.h - 2.0);
                cairo_fill(cr);
            }
            cairo_set_source_rgb(cr, 0.88, 0.89, 0.92);
            if (item.checked) { // a tick
                cairo_set_line_width(cr, 1.8);
                cairo_move_to(cr, 8.0, middle);
                cairo_line_to(cr, 11.0, middle + 3.0);
                cairo_line_to(cr, 16.0, middle - 4.0);
                cairo_stroke(cr);
                cairo_set_line_width(cr, 1.0);
            }
            cairo_move_to(cr, 22.0, middle - (extents.ascent + extents.descent) * 0.5 + extents.ascent);
            cairo_show_text(cr, item.label.c_str());
        }
        cairo_destroy(cr);
        cairo_surface_flush(surface_);
    }

    Display *display_;
    std::vector<MenuItem> items_;
    std::vector<Rect> rows_;
    Window win_ = 0;
    cairo_surface_t *surface_ = nullptr;
    int x_ = 0, y_ = 0, w_ = 0, h_ = 0, hot_ = -1;
    int result_ = 0;
    uint64_t opened_ = 0;
    bool pressedInside_ = false, done_ = false;
};

// --- the editor ----------------------------------------------------------------------------------------------------

// The desktop's scale, as the editor opens at it: the size a host asks for before the window comes is at it already.
float Editor::systemScale() { return screenSize().scale; }

Editor::Editor(Controller &controller) : controller_(controller), view_(controller) {
    dialogs_ = std::make_unique<Dialogs>();
    dialogs_->fallback = [this](const std::string &text) { notify(text); };
    dpiScale_ = systemScale();
    width_ = static_cast<uint32_t>(std::lround(View::kWidth * controller_.layout().zoom * dpiScale_));
    height_ = 0;
    fit(width_, height_);
    keepOnScreen(width_, height_); // a saved size that no longer fits opens within the screen
    askedWidth_ = width_;
}

Editor::~Editor() {
    menu_.reset();
    if (dialogs_)
        dialogs_->cancel();
    view_.endTyping(false);
    releaseKeyFocus();
    if (back_) {
        cairo_surface_destroy(back_);
        back_ = nullptr;
    }
    if (surface_) {
        cairo_surface_destroy(surface_);
        surface_ = nullptr;
    }
    if (resized_) { // the size is the user's, so the next instance opens at it
        userSettings.zoom = controller_.layout().zoom;
        writeUserSettings();
    }
    if (display_) {
        if (window_)
            XDestroyWindow(display_, window_);
        for (unsigned long cursor : cursors_)
            if (cursor)
                XFreeCursor(display_, cursor);
        // The errors still on their way come back now, while the display is ours and they are ignored: a window the
        // host destroyed first answers BadWindow, which the host's handler (Xlib's ends the process) mustn't see.
        XSync(display_, False);
        std::erase(ourDisplays_, display_);
        XCloseDisplay(display_);
        display_ = nullptr;
        // The host's handler goes back once no editor of ours is left, unless something else took the place meanwhile.
        if (ourDisplays_.empty()) {
            int (*current)(Display *, XErrorEvent *) = XSetErrorHandler(hostXErrorHandler_);
            if (current != ourXError)
                XSetErrorHandler(current); // another's handler was on top: it stays, not ours to put back
        }
    }
    window_ = 0;
}

void Editor::size(uint32_t &width, uint32_t &height) const {
    width = width_;
    height = height_;
}

void Editor::fit(uint32_t &width, uint32_t &height) const {
    constrain(width, height, view_.height(), width_, height_);
}

void Editor::fitLayout() {
    uint32_t width = width_;
    uint32_t height = 0;
    fit(width, height);
    keepOnScreen(width, height);
    askedWidth_ = width; // a size the screen held down is no new zoom when the host sets it
    if (width != width_ || height != height_)
        controller_.requestEditorSize(width, height);
}

bool Editor::setSize(uint32_t width, uint32_t height) {
    fit(width, height);
    width_ = width;
    height_ = height;
    // A size the editor asked for, held down to the screen or changed with the DPI, keeps the zoom as it was; a size
    // the host chose, as a window dragged, is the zoom now.
    if (width != askedWidth_) {
        const float zoom = static_cast<float>(width) / (View::kWidth * dpiScale_);
        resized_ = resized_ || std::fabs(zoom - controller_.layout().zoom) > 1e-4f;
        controller_.layout().zoom = zoom;
        askedWidth_ = width;
    }
    if (display_ && window_) {
        // A host that sets the size again and again (as some do each block) leaves the frame and its buffer as they are.
        if (!back_ || width != static_cast<uint32_t>(cairo_image_surface_get_width(back_)) ||
            height != static_cast<uint32_t>(cairo_image_surface_get_height(back_))) {
            XResizeWindow(display_, window_, width, height);
            if (surface_)
                cairo_xlib_surface_set_size(surface_, static_cast<int>(width), static_cast<int>(height));
            backValid_ = false;
            invalidate();
            XFlush(display_);
        }
    }
    return true;
}

bool Editor::attach(WindowId parent) {
    if (window_ || !parent)
        return false;
    display_ = XOpenDisplay(nullptr);
    if (!display_)
        return false;
    ourDisplays_.push_back(display_);
    if (int (*previous)(Display *, XErrorEvent *) = XSetErrorHandler(ourXError); previous != ourXError)
        hostXErrorHandler_ = previous; // the host's, kept for errors that aren't ours
    // The host's window may be of another visual and depth than the screen's default (a 32-bit one, say): the child
    // takes its parent's, and the surface is made for that same visual, or the drawing would not match the window.
    XWindowAttributes parentAttributes{};
    Visual *visual = DefaultVisual(display_, DefaultScreen(display_));
    int depth = DefaultDepth(display_, DefaultScreen(display_));
    if (XGetWindowAttributes(display_, parent, &parentAttributes)) {
        visual = parentAttributes.visual;
        depth = parentAttributes.depth;
    }
    XSetWindowAttributes attributes{};
    attributes.event_mask = kWindowEvents;
    attributes.background_pixmap = None;
    window_ = XCreateWindow(display_, parent, 0, 0, width_, height_, 0, depth, InputOutput, visual,
                            CWEventMask | CWBackPixmap, &attributes);
    if (!window_) {
        XSync(display_, False); // its errors, as in ~Editor(), while they are still ignored
        std::erase(ourDisplays_, display_);
        XCloseDisplay(display_);
        display_ = nullptr;
        return false;
    }
    surface_ = cairo_xlib_surface_create(display_, window_, visual, static_cast<int>(width_), static_cast<int>(height_));
    XMapWindow(display_, window_);
    XFlush(display_);
    invalidate();
    // The editor opened at the desktop's scale (systemScale()), unless the host gave one (CLAP's set_scale, VST3's
    // content scale): only a desktop scaled otherwise by now has the host asked for another size.
    if (!hostScaled_)
        followScale(desktopScale(display_), false);
    return true;
}

void Editor::setScale(double scale) {
    if (scale > 0.0)
        followScale(static_cast<float>(scale), true);
}

// The editor at another scale, its zoom kept. Before its window is made, the size is just its own: the host asks for
// it (get_size, getSize()) and makes the window at it. After, the host is asked for it.
void Editor::followScale(float scale, bool fromHost) {
    hostScaled_ = hostScaled_ || fromHost;
    if (scale <= 0.05f || std::fabs(scale - dpiScale_) < 1e-3f)
        return;
    const float previous = dpiScale_;
    dpiScale_ = scale;
    uint32_t width = static_cast<uint32_t>(std::lround(width_ * dpiScale_ / previous));
    uint32_t height = 0;
    fit(width, height);
    keepOnScreen(width, height);
    askedWidth_ = width; // the host then sets it (setSize()), the zoom kept
    if (!window_) {
        width_ = width;
        height_ = height;
        return;
    }
    if (width != width_ || height != height_)
        controller_.requestEditorSize(width, height);
}

bool Editor::show(bool visible) {
    if (!display_ || !window_)
        return false;
    if (visible)
        XMapWindow(display_, window_);
    else
        XUnmapWindow(display_, window_);
    XFlush(display_);
    return true;
}

void Editor::choosePreset(PresetDialog dialog) { pendingPreset_ = static_cast<int>(dialog); }

bool Editor::modal() const { return menu_ || (dialogs_ && dialogs_->busy()); }

void Editor::notify(std::string text) {
    notice_ = std::move(text);
    noticeUntil_ = nowMs() + 5000; // as oxo synth2's flash: a message a moment, then gone
    invalidate();
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
    whole_ = true;
    areas_.clear();
}

void Editor::invalidate(const std::vector<Rect> &areas) {
    if (whole_)
        return;
    for (const Rect &area : areas)
        areas_.push_back(area);
}

void Editor::pump() {
    pumpEvents();
    if (dialogs_)
        dialogs_->poll();
    // The directory dialog opens once the mouse gesture that asked for it is over: from here, after the events.
    if (pendingPreset_ >= 0 && dialogs_ && !dialogs_->busy()) {
        const auto dialog = static_cast<PresetDialog>(pendingPreset_);
        pendingPreset_ = -1;
        startPresetDialog(dialog);
    }
    tick();
    paint();
}

void Editor::startPresetDialog(PresetDialog what) {
    if (!dialogs_)
        return;
    const bool save = what == PresetDialog::Save, sysex = what == PresetDialog::ImportDx7;
    PresetStore &store = controller_.presetStore();
    store.rescanPresets();
    const std::string folder = utf8Text(store.presetFolder());
    const std::string name = save && store.presetIndex() >= 0 ? store.presetName() : std::string();
    const std::string title = tr(save ? Str::DialogSavePreset : sysex ? Str::DialogImportDx7 : Str::DialogLoadPreset);
    auto then = [this, save, sysex](const std::string &path) {
        if (path.empty() || !dialogs_)
            return;
        PresetStore &store = controller_.presetStore();
        if (save) {
            if (!store.savePreset(widePath(path)))
                dialogs_->message("FMVibe", tr(Str::DialogWriteFailed), true, false);
        } else if (sysex ? !store.importDx7(widePath(path)) : !store.loadPreset(widePath(path))) {
            dialogs_->message("FMVibe", tr(sysex ? Str::DialogNotDx7 : Str::DialogNotPreset), true, false);
        }
        invalidate();
    };
    if (save)
        dialogs_->saveFile(title, folder, name, then);
    else
        dialogs_->openFile(title, folder, sysex, then);
}

void Editor::pumpEvents() {
    if (!display_)
        return;
    auto finishMenu = [this] {
        if (!menu_ || !menu_->done())
            return;
        const int command = menu_->result();
        menu_.reset();
        if (command > 0)
            view_.menuCommand(command);
        invalidate();
    };
    finishMenu(); // a menu whose grab failed is done already, with no event of its own to say so
    while (XPending(display_) > 0) {
        XEvent event{};
        XNextEvent(display_, &event);
        if (menu_) {
            const bool took = menu_->handle(event);
            finishMenu();
            if (took)
                continue;
            // An event the menu doesn't take (the editor's own resize or expose, a clipboard request) goes on.
        }
        handleEvent(&event);
    }
}

void Editor::handleEvent(void *raw) {
    XEvent &event = *static_cast<XEvent *>(raw);
    if (event.xany.window != window_)
        return;
    switch (event.type) {
    case Expose:
        expose_ = true; // what the back buffer holds, copied in again: no redraw
        break;
    case ConfigureNotify:
        if (static_cast<uint32_t>(event.xconfigure.width) != width_ ||
            static_cast<uint32_t>(event.xconfigure.height) != height_) {
            width_ = static_cast<uint32_t>(std::max(event.xconfigure.width, 1));
            height_ = static_cast<uint32_t>(std::max(event.xconfigure.height, 1));
            if (surface_)
                cairo_xlib_surface_set_size(surface_, static_cast<int>(width_), static_cast<int>(height_));
            backValid_ = false;
            invalidate();
        }
        break;
    case ButtonPress: {
        if (event.xbutton.button == Button1)
            takeKeyFocus();
        if (modal())
            return;
        const Point p = toView(event.xbutton.x, event.xbutton.y);
        const Modifiers mods = modifiersOf(event.xbutton.state);
        switch (event.xbutton.button) {
        case Button1: {
            const bool twice = event.xbutton.time - lastClickMs_ < 400 &&
                               std::abs(event.xbutton.x - lastClickX_) < 5 &&
                               std::abs(event.xbutton.y - lastClickY_) < 5;
            lastClickMs_ = event.xbutton.time;
            lastClickX_ = event.xbutton.x;
            lastClickY_ = event.xbutton.y;
            if (twice)
                view_.doubleClick(p, mods);
            else
                view_.mouseDown(p, mods);
            XGrabPointer(display_, window_, False, PointerMotionMask | ButtonPressMask | ButtonReleaseMask,
                         GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
            dragging_ = true;
            break;
        }
        case Button2:
            view_.middleDown(p);
            XGrabPointer(display_, window_, False, PointerMotionMask | ButtonPressMask | ButtonReleaseMask,
                         GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
            dragging_ = true;
            break;
        case Button3:
            contextMenu(event.xbutton.x_root, event.xbutton.y_root, event.xbutton.x, event.xbutton.y);
            break;
        case Button4:
            view_.wheel(p, 1.0f, mods);
            break;
        case Button5:
            view_.wheel(p, -1.0f, mods);
            break;
        case 6:
            view_.wheelAcross(p, -1.0f);
            break;
        case 7:
            view_.wheelAcross(p, 1.0f);
            break;
        default:
            break;
        }
        updateCursor();
        invalidate();
        break;
    }
    case ButtonRelease: {
        const Point p = toView(event.xbutton.x, event.xbutton.y);
        const unsigned button = event.xbutton.button;
        if (button == Button1)
            view_.mouseUp(p, modifiersOf(event.xbutton.state));
        else if (button == Button2)
            view_.middleUp(p);
        // The grab goes once the buttons that drag are all up: not as the wheel turns (a press and a release each
        // notch) or the other of them goes up under a drag, which would leave the drag without its button-up.
        const unsigned released = button == Button1 ? Button1Mask : button == Button2 ? Button2Mask : 0;
        const unsigned held = event.xbutton.state & (Button1Mask | Button2Mask); // as they were before this release
        if (dragging_ && released && (held & ~released) == 0) {
            XUngrabPointer(display_, CurrentTime);
            dragging_ = false;
        }
        updateCursor();
        invalidate();
        break;
    }
    case MotionNotify: {
        XEvent latest;
        while (XCheckTypedWindowEvent(display_, window_, MotionNotify, &latest))
            event = latest; // the latest only
        if (modal())
            return;
        if (view_.mouseMove(toView(event.xmotion.x, event.xmotion.y), modifiersOf(event.xmotion.state)))
            invalidate();
        updateCursor();
        break;
    }
    case LeaveNotify:
        if (dragging_ && event.xcrossing.mode == NotifyGrab) {
            // Another window took the pointer mid-drag (a host's menu or dialog): the gesture ends where it is, but
            // nothing is dropped where the mouse happens to be, as a Win32 window's lost capture does.
            dragging_ = false;
            view_.cancelDrag();
            invalidate();
            break;
        }
        if (dragging_) // the pointer is grabbed: a drag that left the window goes on
            break;
        view_.mouseLeave();
        if (!wantsKeyboard())
            releaseKeyFocus();
        invalidate();
        break;
    case FocusIn:
        hasKeyboard_ = true;
        break;
    case FocusOut:
        hasKeyboard_ = false;
        if (view_.typing()) { // a value typed so far counts, as a Win32 window's WM_KILLFOCUS does
            view_.endTyping(true);
            invalidate();
        }
        break;
    case KeyPress:
        if (modal()) {
            if (XLookupKeysym(&event.xkey, 0) == XK_Escape)
                dialogs_->close();
            invalidate();
            break;
        }
        if (key(&event.xkey)) {
            invalidate();
            return; // the host doesn't see it
        }
        // A key the editor doesn't take goes on to the host's window, which keeps the keyboard for its own shortcuts.
        passKeyToHost(&event);
        break;
    case KeyRelease:
        if (XLookupKeysym(&event.xkey, 0) == XK_Control_L || XLookupKeysym(&event.xkey, 0) == XK_Control_R)
            if (view_.setCtrl(false))
                invalidate();
        break;
    case SelectionRequest:
        answerSelection(&event.xselectionrequest);
        break;
    case SelectionNotify:
        if (clipboardAsked_ && event.xselection.property == None) {
            if (clipboardTarget_ != XA_STRING) {
                // The owner has no UTF8_STRING: ask for plain STRING instead, as older programs offer.
                clipboardTarget_ = XA_STRING;
                XConvertSelection(display_, XInternAtom(display_, "CLIPBOARD", False), clipboardTarget_,
                                  XInternAtom(display_, "FMVIBE_PASTE", False), window_, CurrentTime);
                XFlush(display_);
            } else {
                clipboardAsked_ = false; // nothing offered: the paste comes to nothing
            }
            break;
        }
        if (clipboardAsked_ && event.xselection.property != None) {
            clipboardAsked_ = false;
            Atom type = None;
            int format = 0;
            unsigned long count = 0, after = 0;
            unsigned char *data = nullptr;
            if (XGetWindowProperty(display_, window_, event.xselection.property, 0, 1 << 20, True, AnyPropertyType, &type,
                                   &format, &count, &after, &data) == Success &&
                data) {
                view_.pasteText(std::string_view(reinterpret_cast<const char *>(data),
                                                 count * static_cast<unsigned long>(format) / 8));
                XFree(data);
                invalidate();
            }
        }
        break;
    default:
        break;
    }
}

bool Editor::key(const void *raw) {
    const XKeyEvent &event = *static_cast<const XKeyEvent *>(raw);
    char text[32] = {};
    KeySym symbol = 0;
    const int length = XLookupString(const_cast<XKeyEvent *>(&event), text, sizeof text - 1, &symbol, nullptr);
    const bool ctrl = (event.state & ControlMask) != 0;
    const bool shift = (event.state & ShiftMask) != 0;
    // The letter a Ctrl shortcut is on, whatever Shift and Caps Lock make of it (XLookupString hands Ctrl+Shift+Z as Z),
    // as Windows' key codes and the browser's have it.
    KeySym letter = symbol, upper = symbol;
    XConvertCase(symbol, &letter, &upper);
    if (ctrl && (symbol == XK_Control_L || symbol == XK_Control_R)) {
        if (view_.setCtrl(true))
            invalidate();
        return true;
    }
    if (view_.typing()) {
        if (ctrl && letter == XK_a) {
            view_.typeKey(View::Key::SelectAll);
            return true;
        }
        if (ctrl && (letter == XK_c || letter == XK_x)) {
            setClipboard(view_.copyText(letter == XK_x));
            return true;
        }
        if (ctrl && letter == XK_v) {
            requestClipboard(false);
            return true;
        }
        if (const std::optional<View::Key> typed = typedKeyOf(symbol, shift)) {
            view_.typeKey(*typed, shift);
            return true;
        }
        for (int i = 0; i < length; ++i) { // ASCII and Latin-1, as the View's typing takes them
            const unsigned char byte = static_cast<unsigned char>(text[i]);
            if (byte >= 0x20 && byte != 0x7F)
                view_.typeChar(static_cast<char32_t>(byte));
        }
        return true; // nothing else reaches the host while typing
    }
    // A dialog open over the editor: Enter confirms it, Escape cancels it. An open list or menu: Escape closes it.
    if (dialogKeyFor(symbol))
        return view_.dialogKey((symbol == XK_Return || symbol == XK_KP_Enter) ? View::Key::Enter : View::Key::Escape);
    if (ctrl && (letter == XK_y || (letter == XK_z && shift))) {
        view_.redo();
        return true;
    }
    if (ctrl && letter == XK_z) {
        view_.undo();
        return true;
    }
    if (const std::optional<View::Key> seq = seqKeyOf(ctrl ? letter : symbol, ctrl))
        return view_.seqKey(*seq, shift);
    return false;
}

bool Editor::dialogKeyFor(int keysym) const {
    const bool enter = keysym == XK_Return || keysym == XK_KP_Enter;
    return (view_.dialogOpen() && (enter || keysym == XK_Escape)) ||
           (keysym == XK_Escape && view_.popupTakesEscape());
}

void Editor::tick() {
    if (!display_)
        return;
    const uint64_t now = nowMs();
    // Ctrl let go, or pressed with the mouse over the editor: while the host has the keyboard no key event comes here,
    // and only the next mouse move would tell the view (whose grid shows while Ctrl is held).
    char keys[32] = {};
    XQueryKeymap(display_, keys);
    const auto down = [&](KeySym symbol) {
        const KeyCode code = XKeysymToKeycode(display_, symbol);
        return code != 0 && (keys[code / 8] & (1 << (code % 8))) != 0;
    };
    const bool ctrl = down(XK_Control_L) || down(XK_Control_R);
    const bool ctrlChanged = view_.setCtrl(ctrl);
    const bool polled = view_.poll();
    if (ctrlChanged)
        invalidate();
    else if (polled)
        invalidate(view_.changedAreas());
    if (!notice_.empty() && now >= noticeUntil_) {
        notice_.clear();
        invalidate();
    }
    // A dialog or a notice shows over the whole editor: the frame under it is painted again with it.
    if (modal() || !notice_.empty())
        invalidate();
}

void Editor::ensureBack() {
    if (back_ && cairo_image_surface_get_width(back_) == static_cast<int>(width_) &&
        cairo_image_surface_get_height(back_) == static_cast<int>(height_))
        return;
    if (back_)
        cairo_surface_destroy(back_);
    back_ = cairo_image_surface_create(CAIRO_FORMAT_RGB24, static_cast<int>(width_), static_cast<int>(height_));
    backValid_ = false;
}

// The view drawn into the back buffer: all of it (area null), or what `area` shows over the frame before.
void Editor::drawBack(const Rect *area) {
    const float s = scale();
    const float offsetX = (width_ - View::kWidth * s) * 0.5f;
    const float offsetY = (height_ - view_.height() * s) * 0.5f;
    cairo_t *cr = cairo_create(back_);
    if (area) {
        cairo_rectangle(cr, area->x * s + offsetX, area->y * s + offsetY, area->w * s, area->h * s);
        cairo_clip(cr);
    } else {
        cairo_rectangle(cr, 0.0, 0.0, width_, height_);
        cairo_clip(cr);
    }
    cairo_translate(cr, offsetX, offsetY);
    cairo_scale(cr, s, s);
    Canvas canvas(cr, resources_);
    if (area)
        view_.paint(canvas, *area);
    else
        view_.paint(canvas);
    if (!area && dialogs_ && dialogs_->busy()) {
        // What the dialog covers: the editor says where it went, as the desktop's window may be behind the host's.
        canvas.fillRect({0.0f, 0.0f, View::kWidth, view_.height()}, Color{0.0f, 0.0f, 0.0f, 0.55f});
        const float boxWidth = 380.0f, boxHeight = 84.0f;
        const Rect box{(View::kWidth - boxWidth) * 0.5f, (view_.height() - boxHeight) * 0.5f, boxWidth, boxHeight};
        canvas.fillRect(box, kPanel.alpha(0.97f), kRadiusPanel);
        canvas.strokeRect(box, kBorder, 1.0f, kRadiusPanel);
        canvas.text({box.x, box.y + 14.0f, box.w, 22.0f}, tr(Str::LinuxDialogOpen), kTextHeading, kText, Align::Center,
                    true);
        canvas.text({box.x, box.y + 44.0f, box.w, 20.0f}, tr(Str::LinuxDialogHint), kTextBody, kDim);
    }
    if (!area && !notice_.empty()) {
        const float boxWidth =
            std::min(View::kWidth - 80.0f, static_cast<float>(canvas.textWidth(notice_, kTextBody)) + 40.0f);
        const Rect box{(View::kWidth - boxWidth) * 0.5f, view_.height() - 64.0f, boxWidth, 32.0f};
        canvas.fillRect(box, kPanel.alpha(0.97f), kRadiusCard);
        canvas.strokeRect(box, kBorder, 1.0f, kRadiusCard);
        canvas.text(box, notice_, kTextBody, kText);
    }
    cairo_destroy(cr);
    cairo_surface_flush(back_);
}

void Editor::paint() {
    if (!display_ || !window_ || !surface_)
        return;
    if (!whole_ && areas_.empty() && !expose_)
        return;
    ensureBack();
    const bool redrawAll = whole_ || !backValid_;
    if (redrawAll) {
        drawBack(nullptr);
        backValid_ = true;
    } else {
        for (const Rect &area : areas_)
            drawBack(&area);
    }
    // One copy of the back buffer into the window, whole or of what changed: the window never shows a frame half drawn.
    const float s = scale();
    const float offsetX = (width_ - View::kWidth * s) * 0.5f;
    const float offsetY = (height_ - view_.height() * s) * 0.5f;
    cairo_t *screen = cairo_create(surface_);
    cairo_set_source_surface(screen, back_, 0.0, 0.0);
    if (redrawAll || expose_) {
        cairo_paint(screen);
    } else {
        for (const Rect &area : areas_) {
            cairo_save(screen);
            cairo_rectangle(screen, area.x * s + offsetX, area.y * s + offsetY, area.w * s, area.h * s);
            cairo_clip(screen);
            cairo_paint(screen);
            cairo_restore(screen);
        }
    }
    cairo_destroy(screen);
    cairo_surface_flush(surface_);
    whole_ = false;
    areas_.clear();
    expose_ = false;
    XFlush(display_);
}

void Editor::updateCursor() {
    if (!display_ || !window_)
        return;
    unsigned shape = XC_left_ptr;
    switch (view_.cursor()) {
    case CursorKind::Pen:
        shape = XC_pencil;
        break;
    case CursorKind::Arrow:
        shape = XC_left_ptr;
        break;
    case CursorKind::Hand:
        shape = XC_hand2;
        break;
    case CursorKind::Vertical:
        shape = XC_sb_v_double_arrow;
        break;
    case CursorKind::Horizontal:
        shape = XC_sb_h_double_arrow;
        break;
    case CursorKind::Move:
        shape = XC_fleur;
        break;
    case CursorKind::Text:
        shape = XC_xterm;
        break;
    }
    const size_t kind = static_cast<size_t>(view_.cursor());
    if (!cursors_[kind])
        cursors_[kind] = XCreateFontCursor(display_, shape);
    if (cursors_[kind])
        XDefineCursor(display_, window_, cursors_[kind]);
}

void Editor::contextMenu(int rootX, int rootY, int x, int y) {
    if (dragging_) { // a right-click under a drag: the menu takes the pointer, so the drag ends here, as on Windows
        XUngrabPointer(display_, CurrentTime);
        dragging_ = false;
        view_.cancelDrag();
    }
    const std::vector<MenuItem> items = view_.contextMenu(toView(x, y));
    if (items.empty() || !display_ || !window_)
        return;
    menu_ = std::make_unique<Menu>(display_, items, rootX, rootY);
}

void Editor::takeKeyFocus() {
    if (!display_ || !window_)
        return;
    Window focus = None;
    int revert = 0;
    XGetInputFocus(display_, &focus, &revert);
    if (focus == window_) {
        hasKeyboard_ = true;
        return;
    }
    keyboardFrom_ = focus;
    XSetInputFocus(display_, window_, RevertToParent, CurrentTime);
    hasKeyboard_ = true;
    XFlush(display_);
}

Editor::WindowId Editor::hostFocusWindow() const {
    if (display_ && keyboardFrom_ > PointerRoot)
        return keyboardFrom_;
    // The host's window around the editor: its parent, which is alive as long as we are.
    Window root = None, parent = None, *children = nullptr;
    unsigned count = 0;
    if (display_ && window_ && XQueryTree(display_, window_, &root, &parent, &children, &count)) {
        if (children)
            XFree(children);
        if (parent && parent != root)
            return parent;
    }
    return keyboardFrom_ ? keyboardFrom_ : None;
}

void Editor::releaseKeyFocus() {
    if (!display_ || !window_ || !hasKeyboard_)
        return;
    Window focus = None;
    int revert = 0;
    XGetInputFocus(display_, &focus, &revert);
    if (focus == window_) {
        const Window host = hostFocusWindow();
        if (host)
            XSetInputFocus(display_, host, RevertToParent, CurrentTime);
    }
    hasKeyboard_ = false;
    keyboardFrom_ = 0;
    XFlush(display_);
}

void Editor::passKeyToHost(const void *raw) {
    if (!display_ || !window_)
        return;
    const Window host = hostFocusWindow();
    if (!host)
        return;
    XEvent copy = *static_cast<const XEvent *>(raw);
    copy.xkey.window = host;
    copy.xkey.subwindow = None;
    releaseKeyFocus();
    XSendEvent(display_, host, True, KeyPressMask, &copy);
    XFlush(display_);
}

void Editor::setClipboard(std::string_view text) {
    clipboard_.assign(text);
    if (!display_ || !window_)
        return;
    XSetSelectionOwner(display_, XInternAtom(display_, "CLIPBOARD", False), window_, CurrentTime);
    XFlush(display_);
}

void Editor::requestClipboard(bool cut) {
    if (cut) { // the view empties what it cuts; its text is what the clipboard holds
        setClipboard(view_.copyText(true));
        return;
    }
    if (!display_ || !window_)
        return;
    clipboardAsked_ = true;
    clipboardTarget_ = XInternAtom(display_, "UTF8_STRING", False);
    XConvertSelection(display_, XInternAtom(display_, "CLIPBOARD", False), clipboardTarget_,
                      XInternAtom(display_, "FMVIBE_PASTE", False), window_, CurrentTime);
    XFlush(display_);
}

void Editor::answerSelection(void *raw) {
    XSelectionRequestEvent &request = *static_cast<XSelectionRequestEvent *>(raw);
    XSelectionEvent reply{};
    reply.type = SelectionNotify;
    reply.display = request.display;
    reply.requestor = request.requestor;
    reply.selection = request.selection;
    reply.target = request.target;
    reply.time = request.time;
    reply.property = None;
    const Atom property = request.property == None ? request.target : request.property;
    const Atom utf8 = XInternAtom(display_, "UTF8_STRING", False);
    const Atom targets = XInternAtom(display_, "TARGETS", False);
    const Atom text = XInternAtom(display_, "TEXT", False);
    if (request.target == targets) {
        const Atom offered[] = {utf8, XA_STRING, text};
        XChangeProperty(display_, request.requestor, property, XA_ATOM, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char *>(offered), 3);
        reply.property = property;
    } else if (request.target == utf8 || request.target == XA_STRING || request.target == text) {
        XChangeProperty(display_, request.requestor, property, request.target, 8, PropModeReplace,
                        reinterpret_cast<const unsigned char *>(clipboard_.data()),
                        static_cast<int>(clipboard_.size()));
        reply.property = property;
    }
    XSendEvent(display_, request.requestor, False, 0, reinterpret_cast<XEvent *>(&reply));
    XFlush(display_);
}

} // namespace fmvibe::gui
