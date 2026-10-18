#pragma once

#include <string>

struct _XDisplay;

namespace clipper {

// window manager + keyboard bits for the popup, on glfw's X connection (ui thread only)
class X11Desktop {
public:
    using WindowId = unsigned long;

    explicit X11Desktop(_XDisplay* display);
    ~X11Desktop();
    X11Desktop(const X11Desktop&) = delete;
    X11Desktop& operator=(const X11Desktop&) = delete;

    WindowId activeWindow() const;  // 0 if the wm doesn't say
    std::string windowClass(WindowId window) const;  // WM_CLASS class part ("Gnome-terminal")
    bool isTerminal(WindowId window) const;          // pastes with Ctrl+Shift+V, so we send Shift+Insert

    // asks the wm to focus it, as a pager would (wms don't refuse those like they do apps)
    void activate(WindowId window, bool raise = true) const;
    bool waitUntilActive(WindowId window, int timeoutMs) const;

    bool pointer(int* x, int* y) const;  // root coordinates

    // before mapping: utility window without taskbar entry, above the others. false = normal window
    void setPopupHints(WindowId window, bool popup) const;

    // presses the paste key in whatever has focus: Ctrl+V, or Shift+Insert for terminals.
    // lets go of modifiers that are still held first (Ctrl+Shift from the shortcut).
    // false if XTest isn't there (libXtst.so.6, loaded at runtime)
    bool canSendKeys() const { return fakeKey_ != nullptr; }
    bool sendPaste(bool terminalStyle) const;

private:
    unsigned long atom(const char* name) const;

    _XDisplay* dpy_;
    void* xtst_ = nullptr;
    using FakeKeyFn = int (*)(_XDisplay*, unsigned int, int, unsigned long);
    FakeKeyFn fakeKey_ = nullptr;
};

}  // namespace clipper
