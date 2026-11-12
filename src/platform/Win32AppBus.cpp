// single instance on windows: the first Clipper.exe holds a named mutex and a hidden window
// (class ClipperAppBus). later launches find that window, send it their request as WM_COPYDATA and exit.
// the same window owns the popup hotkey and quits the app when windows / an installer asks
// (WM_ENDSESSION, also what the Restart Manager sends to close us for an update)

#include <future>
#include <memory>
#include <string_view>

#include "platform/AppBus.h"
#include "platform/Win32Util.h"

namespace clipper {

namespace {

constexpr const wchar_t* kMutexName = L"Local\\io.github.KimsFerdy.Clipper";
constexpr const wchar_t* kClassName = L"ClipperAppBus";
constexpr ULONG_PTR kCopyDataTag = 0x434C4950;  // "CLIP"
constexpr int kHotKeyId = 1;
constexpr UINT kMsgStop = WM_APP + 1;

std::atomic<HWND> g_window{nullptr};

const char* kindName(AppBus::Request::Kind k) {
    switch (k) {
        case AppBus::Request::Kind::Toggle:
            return "toggle";
        case AppBus::Request::Kind::Popup:
            return "popup";
        case AppBus::Request::Kind::Quit:
            return "quit";
        case AppBus::Request::Kind::Show:
            break;
    }
    return "show";
}

}  // namespace

HWND win32::appBusWindow() { return g_window; }

struct AppBusWin32 {
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        }
        auto* bus = reinterpret_cast<AppBus*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!bus) return DefWindowProcW(hwnd, msg, wp, lp);
        switch (msg) {
            case WM_COPYDATA: {  // "<kind>\n<search>" from another launch
                const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(lp);
                if (cds->dwData != kCopyDataTag) return FALSE;
                const std::string_view text(static_cast<const char*>(cds->lpData), cds->cbData);
                const size_t nl = text.find('\n');
                const std::string_view k = text.substr(0, nl);
                AppBus::Request req;
                req.kind = k == "toggle"  ? AppBus::Request::Kind::Toggle
                           : k == "popup" ? AppBus::Request::Kind::Popup
                           : k == "quit"  ? AppBus::Request::Kind::Quit
                                          : AppBus::Request::Kind::Show;
                if (nl != std::string_view::npos) req.search = std::string(text.substr(nl + 1));
                bus->push(std::move(req));
                return TRUE;
            }
            case WM_HOTKEY:
                if (wp == kHotKeyId) bus->push({AppBus::Request::Kind::Popup, {}});
                return 0;
            case win32::kMsgSetHotKey:
                UnregisterHotKey(hwnd, kHotKeyId);
                if (!lp) return 1;
                return RegisterHotKey(hwnd, kHotKeyId, static_cast<UINT>(wp) | MOD_NOREPEAT, static_cast<UINT>(lp)) ? 1 : 0;
            case WM_QUERYENDSESSION:
                return TRUE;
            case WM_ENDSESSION:
                if (wp) bus->push({AppBus::Request::Kind::Quit, {}});
                return 0;
            case WM_CLOSE:  // someone else (taskkill without /f) asking us to go
                bus->push({AppBus::Request::Kind::Quit, {}});
                return 0;
            case kMsgStop:
                DestroyWindow(hwnd);
                return 0;
            case WM_DESTROY:
                UnregisterHotKey(hwnd, kHotKeyId);
                g_window = nullptr;
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
};

AppBus::AppBus(const Request& launch, std::function<void()> wake) : wake_(std::move(wake)) {
    HANDLE mutex = CreateMutexW(nullptr, FALSE, kMutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        // someone's already running: hand them the request and get out
        primary_ = false;
        HWND other = nullptr;
        for (int i = 0; i < 40 && !(other = FindWindowW(kClassName, nullptr)); ++i) Sleep(50);  // still starting up
        if (other) {
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            AllowSetForegroundWindow(pid);  // we got launched by the user, they may come to the front
            const std::string payload = std::string(kindName(launch.kind)) + "\n" + launch.search;
            COPYDATASTRUCT cds{kCopyDataTag, static_cast<DWORD>(payload.size()), const_cast<char*>(payload.data())};
            SendMessageTimeoutW(other, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds), SMTO_ABORTIFHUNG, 3000, nullptr);
        }
        CloseHandle(mutex);
        return;
    }
    instanceMutex_ = mutex;

    auto ready = std::make_shared<std::promise<void>>();
    std::future<void> started = ready->get_future();
    thread_ = std::thread([this, ready] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = AppBusWin32::proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);
        // a real top-level window (never shown), message-only ones don't get WM_ENDSESSION
        HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kClassName, L"Clipper instance", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                    wc.hInstance, this);
        hwnd_ = hwnd;
        g_window = hwnd;
        ready->set_value();
        if (!hwnd) return;
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
    });
    started.wait();
}

AppBus::~AppBus() {
    if (HWND hwnd = static_cast<HWND>(hwnd_.load())) PostMessageW(hwnd, kMsgStop, 0, 0);
    if (thread_.joinable()) thread_.join();
    if (instanceMutex_) CloseHandle(instanceMutex_);
}

std::vector<AppBus::Request> AppBus::takeRequests() {
    std::lock_guard lock(mutex_);
    return std::exchange(requests_, {});
}

void AppBus::push(Request request) {
    {
        std::lock_guard lock(mutex_);
        requests_.push_back(std::move(request));
    }
    if (wake_) wake_();
}

void AppBus::run() {}  // the thread lambda above is the loop

}  // namespace clipper
