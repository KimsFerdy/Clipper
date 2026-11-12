#pragma once

#include <cstdint>
#include <string>

struct _XDisplay;

namespace clipper {

// window manager + keyboard bits for the popup, ui thread only.
// linux: X11Desktop.cpp, on glfw's X connection. windows: Win32Desktop.cpp
class Desktop {
public:
    using WindowId = std::uintptr_t;  // X11 Window / HWND

#ifdef _WIN32
    Desktop() = default;
#else
    explicit Desktop(_XDisplay* display);
#endif
    ~Desktop();
    Desktop(const Desktop&) = delete;
    Desktop& operator=(const Desktop&) = delete;

    WindowId activeWindow() const;  // 0 if the wm doesn't say
    std::string windowClass(WindowId window) const;  // WM_CLASS class part ("Gnome-terminal") / exe name ("mintty")
    bool isTerminal(WindowId window) const;          // pastes with Ctrl+Shift+V, so we send Shift+Insert

    // asks the wm to focus it, as a pager would (wms don't refuse those like they do apps)
    void activate(WindowId window, bool raise = true) const;
    bool waitUntilActive(WindowId window, int timeoutMs) const;

    bool pointer(int* x, int* y) const;  // root / screen coordinates

    // before mapping: utility window without taskbar entry, above the others. false = normal window
    void setPopupHints(WindowId window, bool popup) const;

    // presses the paste key in whatever has focus: Ctrl+V, or Shift+Insert for terminals.
    // lets go of modifiers that are still held first (Ctrl+Shift from the shortcut).
    // linux: false if XTest isn't there (libXtst.so.6, loaded at runtime)
#ifdef _WIN32
    bool canSendKeys() const { return true; }  // SendInput
#else
    bool canSendKeys() const { return fakeKey_ != nullptr; }
#endif
    bool sendPaste(bool terminalStyle) const;

private:
#ifndef _WIN32
    unsigned long atom(const char* name) const;

    _XDisplay* dpy_;
    void* xtst_ = nullptr;
    using FakeKeyFn = int (*)(_XDisplay*, unsigned int, int, unsigned long);
    FakeKeyFn fakeKey_ = nullptr;
#endif
};

}  // namespace clipper
