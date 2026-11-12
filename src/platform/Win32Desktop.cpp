#include "platform/Win32Util.h"  // windows.h, before the other windows headers

#include <dwmapi.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>
#include <vector>

#include "platform/Desktop.h"

namespace clipper {

namespace {

HWND hwndOf(Desktop::WindowId window) { return reinterpret_cast<HWND>(window); }

bool extended(WORD vk) {
    switch (vk) {
        case VK_RCONTROL:
        case VK_RMENU:
        case VK_LWIN:
        case VK_RWIN:
        case VK_INSERT:
            return true;
        default:
            return false;
    }
}

void key(std::vector<INPUT>* out, WORD vk, bool up) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    in.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (extended(vk) ? KEYEVENTF_EXTENDEDKEY : 0);
    out->push_back(in);
}

bool down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

}  // namespace

Desktop::~Desktop() = default;

Desktop::WindowId Desktop::activeWindow() const { return reinterpret_cast<WindowId>(GetForegroundWindow()); }

std::string Desktop::windowClass(WindowId window) const { return win32::windowProcessName(hwndOf(window)); }

bool Desktop::isTerminal(WindowId window) const {
    // windows terminal and the classic console paste with Ctrl+V. these only do Shift+Insert / Ctrl+Shift+V
    std::string name = windowClass(window);
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* t : {"mintty", "putty", "kitty", "alacritty", "wezterm-gui"}) {
        if (name == t) return true;
    }
    return false;
}

void Desktop::activate(WindowId window, bool /*raise*/) const {
    HWND h = hwndOf(window);
    if (!IsWindow(h)) return;
    if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
    if (SetForegroundWindow(h)) return;
    // windows only lets the foreground process hand focus around. borrow its input queue for a moment
    const DWORD me = GetCurrentThreadId();
    const DWORD fg = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    if (fg && fg != me && AttachThreadInput(me, fg, TRUE)) {
        SetForegroundWindow(h);
        BringWindowToTop(h);
        AttachThreadInput(me, fg, FALSE);
    }
}

bool Desktop::waitUntilActive(WindowId window, int timeoutMs) const {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        if (GetForegroundWindow() == hwndOf(window)) return true;
        if (std::chrono::steady_clock::now() >= until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

bool Desktop::pointer(int* x, int* y) const {
    POINT p;
    if (!GetCursorPos(&p)) return false;
    *x = p.x;
    *y = p.y;
    return true;
}

void Desktop::setPopupHints(WindowId window, bool popup) const {
    HWND h = hwndOf(window);
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    ex = popup ? (ex | WS_EX_TOOLWINDOW) & ~static_cast<LONG_PTR>(WS_EX_APPWINDOW)
               : (ex | WS_EX_APPWINDOW) & ~static_cast<LONG_PTR>(WS_EX_TOOLWINDOW);
    SetWindowLongPtrW(h, GWL_EXSTYLE, ex);
    SetWindowPos(h, popup ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    // windows 11: round corners on the frameless popup too, like a menu. ignored on 10
    const DWORD corners = popup ? 3 /*DWMWCP_ROUNDSMALL*/ : 0 /*DWMWCP_DEFAULT*/;
    DwmSetWindowAttribute(h, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corners, sizeof corners);
}

bool Desktop::sendPaste(bool terminalStyle) const {
    std::vector<INPUT> in;
    // let go of what's still held from the shortcut / Ctrl+1..9. alt and win get a ctrl tap first,
    // releasing them alone would open the menu bar / the start menu
    constexpr WORD kHeld[] = {VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL};
    constexpr WORD kTapFirst[] = {VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN};
    for (WORD vk : kHeld) {
        if (down(vk)) key(&in, vk, true);
    }
    for (WORD vk : kTapFirst) {
        if (!down(vk)) continue;
        key(&in, VK_CONTROL, false);
        key(&in, vk, true);
        key(&in, VK_CONTROL, true);
    }
    const WORD mod = terminalStyle ? VK_SHIFT : VK_CONTROL;
    const WORD k = terminalStyle ? VK_INSERT : 'V';
    key(&in, mod, false);
    key(&in, k, false);
    key(&in, k, true);
    key(&in, mod, true);
    return SendInput(static_cast<UINT>(in.size()), in.data(), sizeof(INPUT)) == in.size();
}

}  // namespace clipper
