#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct sd_bus;

namespace clipper {

// single instance + remote control.
// linux (AppBus.cpp): the first instance owns io.github.KimsFerdy.Clipper on the session bus, later launches
// (launcher click, the popup shortcut) just send it a message and exit.
// windows (Win32AppBus.cpp): a named mutex + a hidden window that takes WM_COPYDATA from later launches.
// that window also gets the popup hotkey (RegisterHotKey) and the installer's "please close" (WM_ENDSESSION)
class AppBus {
public:
    struct Request {
        enum class Kind { Show, Toggle, Popup, Quit } kind = Kind::Show;
        std::string search;  // "" = keep the current filter
    };

    // primary: owns the name and serves requests on its own thread.
    // otherwise the request goes to the running instance and primary() is false
    AppBus(const Request& launch, std::function<void()> wake);
    ~AppBus();
    AppBus(const AppBus&) = delete;
    AppBus& operator=(const AppBus&) = delete;

    bool primary() const { return primary_; }
    std::vector<Request> takeRequests();

private:
    friend struct AppBusDbus;
    friend struct AppBusWin32;
    void run();
    void push(Request request);

    std::function<void()> wake_;
#ifdef _WIN32
    void* instanceMutex_ = nullptr;
    std::atomic<void*> hwnd_{nullptr};
#else
    sd_bus* bus_ = nullptr;
#endif
    bool primary_ = true;
    std::atomic<bool> stop_{false};
    std::mutex mutex_;
    std::vector<Request> requests_;
    std::thread thread_;
};

}  // namespace clipper
