// tray icon on windows: Shell_NotifyIcon from a hidden window on its own thread.
// left click = Toggle (the manager), right click = the menu. explorer restarting (or starting after us at
// login) broadcasts TaskbarCreated, the icon gets added again then

#include "platform/Win32Util.h"  // windows.h, before the other windows headers

#include <shellapi.h>
#include <windowsx.h>

#include <future>
#include <memory>

#include "platform/Tray.h"

namespace clipper {

namespace {

constexpr UINT kMsgNotify = WM_APP + 1;
constexpr UINT kMsgStop = WM_APP + 2;
constexpr UINT kMsgPaused = WM_APP + 3;
constexpr UINT kIconId = 1;
constexpr UINT_PTR kRetryTimer = 1;
enum MenuId : UINT { kOpen = 1, kPause, kQuit };

// dark context menu when windows is dark. uxtheme's SetPreferredAppMode (ordinal 135, windows 10 1903+)
// is undocumented but what notepad++ and friends use. nothing happens if it's not there
void allowDarkMenus() {
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    using SetPreferredAppMode = int(WINAPI*)(int);
    if (auto set = reinterpret_cast<SetPreferredAppMode>(GetProcAddress(ux, MAKEINTRESOURCEA(135)))) set(1 /*AllowDark*/);
}

}  // namespace

struct TrayWin32 {
    Tray& tray;
    HWND hwnd = nullptr;
    HICON icon = nullptr;
    UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    NOTIFYICONDATAW data() const {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof nid;
        nid.hWnd = hwnd;
        nid.uID = kIconId;
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        nid.uCallbackMessage = kMsgNotify;
        nid.hIcon = icon;
        wcscpy_s(nid.szTip, tray.paused_ ? L"Clipper (paused)" : L"Clipper");
        return nid;
    }

    void add() {
        NOTIFYICONDATAW nid = data();
        Shell_NotifyIconW(NIM_DELETE, &nid);  // explorer restarted: there may be a dead one
        if (Shell_NotifyIconW(NIM_ADD, &nid)) {
            nid.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &nid);
            tray.available_ = true;
            KillTimer(hwnd, kRetryTimer);
        } else {
            tray.available_ = false;  // no taskbar yet (early at login), try again in a bit
            SetTimer(hwnd, kRetryTimer, 2000, nullptr);
        }
    }

    void menu(int x, int y) {
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, kOpen, L"Open Clipper");
        AppendMenuW(m, MF_STRING | (tray.paused_ ? MF_CHECKED : 0), kPause, L"Pause capturing");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, kQuit, L"Quit");
        SetMenuDefaultItem(m, kOpen, FALSE);
        SetForegroundWindow(hwnd);  // or the menu doesn't go away when clicking elsewhere
        const UINT align = GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
        const UINT cmd = static_cast<UINT>(
            TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | align | TPM_BOTTOMALIGN, x, y, hwnd, nullptr));
        PostMessageW(hwnd, WM_NULL, 0, 0);
        DestroyMenu(m);
        if (cmd == kOpen) tray.push(Tray::Action::Show);
        if (cmd == kPause) tray.push(Tray::Action::TogglePause);
        if (cmd == kQuit) tray.push(Tray::Action::Quit);
    }

    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        }
        auto* self = reinterpret_cast<TrayWin32*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        if (msg == self->taskbarCreated) {
            self->add();
            return 0;
        }
        switch (msg) {
            case kMsgNotify:  // NOTIFYICON_VERSION_4: event in LOWORD(lp), anchor point in wp
                switch (LOWORD(lp)) {
                    case NIN_SELECT:
                    case NIN_KEYSELECT:
                        self->tray.push(Tray::Action::Toggle);
                        break;
                    case WM_CONTEXTMENU:
                        self->menu(GET_X_LPARAM(wp), GET_Y_LPARAM(wp));
                        break;
                }
                return 0;
            case kMsgPaused: {
                NOTIFYICONDATAW nid = self->data();
                nid.uFlags = NIF_TIP | NIF_SHOWTIP;
                Shell_NotifyIconW(NIM_MODIFY, &nid);
                return 0;
            }
            case WM_TIMER:
                if (wp == kRetryTimer) self->add();
                return 0;
            case kMsgStop: {
                NOTIFYICONDATAW nid = self->data();
                Shell_NotifyIconW(NIM_DELETE, &nid);
                DestroyWindow(hwnd);
                return 0;
            }
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
};

Tray::Tray(std::vector<Icon> icons, std::function<void()> wake) : icons_(std::move(icons)), wake_(std::move(wake)) {
    auto ready = std::make_shared<std::promise<void>>();
    std::future<void> started = ready->get_future();
    thread_ = std::thread([this, ready] {
        allowDarkMenus();
        TrayWin32 self{*this};
        const HINSTANCE inst = GetModuleHandleW(nullptr);
        const int size = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem());
        self.icon = static_cast<HICON>(LoadImageW(inst, L"GLFW_ICON", IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
        if (!self.icon) self.icon = LoadIconW(nullptr, IDI_APPLICATION);

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = TrayWin32::proc;
        wc.hInstance = inst;
        wc.lpszClassName = L"ClipperTray";
        RegisterClassExW(&wc);
        // top-level (never shown): TaskbarCreated is a broadcast, message-only windows miss it
        self.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Clipper tray", WS_POPUP, 0, 0, 0, 0, nullptr,
                                    nullptr, inst, &self);
        hwnd_ = self.hwnd;
        if (self.hwnd) self.add();
        ready->set_value();
        if (self.hwnd) {
            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
        }
        hwnd_ = nullptr;
        available_ = false;
        if (self.icon) DestroyIcon(self.icon);
    });
    started.wait();
}

Tray::~Tray() {
    stop_ = true;
    if (HWND hwnd = static_cast<HWND>(hwnd_.load())) PostMessageW(hwnd, kMsgStop, 0, 0);
    if (thread_.joinable()) thread_.join();
}

void Tray::setPaused(bool paused) {
    if (paused_.exchange(paused) == paused) return;
    if (HWND hwnd = static_cast<HWND>(hwnd_.load())) PostMessageW(hwnd, kMsgPaused, 0, 0);
}

std::vector<Tray::Action> Tray::takeActions() {
    std::lock_guard lock(mutex_);
    return std::exchange(actions_, {});
}

void Tray::push(Action action) {
    {
        std::lock_guard lock(mutex_);
        actions_.push_back(action);
    }
    if (wake_) wake_();
}

void Tray::run() {}  // the thread lambda above is the loop

}  // namespace clipper
