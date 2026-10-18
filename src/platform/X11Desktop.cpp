#include "platform/X11Desktop.h"

#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <string_view>
#include <vector>

namespace clipper {

namespace {

std::string toLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// WM_CLASS of the terminals people actually use. they all paste CLIPBOARD on Ctrl+Shift+V
// (which is our shortcut now) and PRIMARY on Shift+Insert
constexpr std::string_view kTerminals[] = {
    "gnome-terminal", "gnome-terminal-server", "xfce4-terminal", "mate-terminal", "konsole", "tilix", "terminator",
    "kitty", "alacritty", "xterm", "uxterm", "urxvt", "rxvt", "lxterminal", "qterminal", "terminology", "guake",
    "tilda", "sakura", "st-256color", "wezterm", "org.wezfurlong.wezterm", "com.mitchellh.ghostty", "ghostty",
    "foot", "kgx", "org.gnome.console", "ptyxis", "cool-retro-term", "deepin-terminal", "termite", "yakuake",
};

}  // namespace

X11Desktop::X11Desktop(_XDisplay* display) : dpy_(display) {
    // libxtst6 is on every desktop install, its -dev package often isn't. dlopen = no build dep
    xtst_ = dlopen("libXtst.so.6", RTLD_LAZY | RTLD_LOCAL);
    if (xtst_) fakeKey_ = reinterpret_cast<FakeKeyFn>(dlsym(xtst_, "XTestFakeKeyEvent"));
}

X11Desktop::~X11Desktop() {
    if (xtst_) dlclose(xtst_);
}

unsigned long X11Desktop::atom(const char* name) const { return XInternAtom(dpy_, name, False); }

X11Desktop::WindowId X11Desktop::activeWindow() const {
    if (!dpy_) return 0;
    Atom type;
    int format;
    unsigned long count, after;
    unsigned char* prop = nullptr;
    WindowId active = 0;
    if (XGetWindowProperty(dpy_, DefaultRootWindow(dpy_), atom("_NET_ACTIVE_WINDOW"), 0, 1, False, XA_WINDOW, &type,
                           &format, &count, &after, &prop) == Success &&
        prop) {
        if (count == 1 && format == 32) active = *reinterpret_cast<unsigned long*>(prop);
        XFree(prop);
    }
    return active;
}

std::string X11Desktop::windowClass(WindowId window) const {
    if (!dpy_ || !window) return {};
    XClassHint hint{};
    std::string out;
    if (XGetClassHint(dpy_, window, &hint)) {
        if (hint.res_class) out = hint.res_class;
        if (hint.res_name) XFree(hint.res_name);
        if (hint.res_class) XFree(hint.res_class);
    }
    return out;
}

bool X11Desktop::isTerminal(WindowId window) const {
    const std::string cls = toLower(windowClass(window));
    if (cls.empty()) return false;
    return std::find(std::begin(kTerminals), std::end(kTerminals), cls) != std::end(kTerminals) ||
           cls.find("terminal") != std::string::npos;
}

void X11Desktop::activate(WindowId window, bool raise) const {
    if (!dpy_ || !window) return;
    XEvent ev{};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = window;
    ev.xclient.message_type = atom("_NET_ACTIVE_WINDOW");
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = 2;  // source: pager
    ev.xclient.data.l[1] = CurrentTime;
    XSendEvent(dpy_, DefaultRootWindow(dpy_), False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    if (raise) XRaiseWindow(dpy_, window);
    XFlush(dpy_);
}

bool X11Desktop::waitUntilActive(WindowId window, int timeoutMs) const {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end) {
        if (activeWindow() == window) return true;
        usleep(5000);
    }
    return false;
}

bool X11Desktop::pointer(int* x, int* y) const {
    if (!dpy_) return false;
    Window rootRet, child;
    int wx, wy;
    unsigned int mask;
    return XQueryPointer(dpy_, DefaultRootWindow(dpy_), &rootRet, &child, x, y, &wx, &wy, &mask);
}

void X11Desktop::setPopupHints(WindowId window, bool popup) const {
    if (!dpy_ || !window) return;
    const Atom type = atom(popup ? "_NET_WM_WINDOW_TYPE_UTILITY" : "_NET_WM_WINDOW_TYPE_NORMAL");
    XChangeProperty(dpy_, window, atom("_NET_WM_WINDOW_TYPE"), XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(&type), 1);
    // read by the wm when the window gets mapped, so this only works while it's hidden
    std::vector<Atom> state;
    if (popup) state = {atom("_NET_WM_STATE_SKIP_TASKBAR"), atom("_NET_WM_STATE_SKIP_PAGER"), atom("_NET_WM_STATE_ABOVE")};
    XChangeProperty(dpy_, window, atom("_NET_WM_STATE"), XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(state.data()), static_cast<int>(state.size()));
    XFlush(dpy_);
}

bool X11Desktop::sendPaste(bool terminalStyle) const {
    if (!dpy_ || !fakeKey_) return false;

    // still holding Ctrl / Shift / Alt / Super from picking the clip? Ctrl+Shift+V would just open us again
    char keys[32];
    XQueryKeymap(dpy_, keys);
    if (XModifierKeymap* mods = XGetModifierMapping(dpy_)) {
        for (int i = 0; i < 8 * mods->max_keypermod; ++i) {
            const KeyCode kc = mods->modifiermap[i];
            if (kc && (keys[kc / 8] & (1 << (kc % 8)))) fakeKey_(dpy_, kc, False, CurrentTime);
        }
        XFreeModifiermap(mods);
    }

    const KeyCode mod = XKeysymToKeycode(dpy_, terminalStyle ? XK_Shift_L : XK_Control_L);
    const KeyCode key = XKeysymToKeycode(dpy_, terminalStyle ? XK_Insert : XK_v);
    if (!mod || !key) return false;
    fakeKey_(dpy_, mod, True, CurrentTime);
    fakeKey_(dpy_, key, True, CurrentTime);
    fakeKey_(dpy_, key, False, CurrentTime);
    fakeKey_(dpy_, mod, False, CurrentTime);
    XSync(dpy_, False);
    return true;
}

}  // namespace clipper
