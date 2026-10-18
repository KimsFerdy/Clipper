#pragma once

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/Clip.h"

namespace clipper {

// what goes on / comes off the clipboard
struct ClipboardData {
    ClipKind kind = ClipKind::Text;
    std::string text;  // text, or file:// uris one per line
    std::string png;
    bool cut = false;    // files
    std::string source;  // captures only: WM_CLASS of the app that had focus
};

// the CLIPBOARD selection, on its own thread with its own X connection:
// - watches it with XFixes (no polling) and reads whatever got copied: text, image/png, or files
//   (x-special/gnome-copied-files, what nemo / nautilus / caja put there)
// - owns it when we paste something from the history and answers other apps' requests,
//   INCR for the big ones (screenshots)
// - keeps the last copy alive when the app it came from quits
class X11Clipboard {
public:
    explicit X11Clipboard(std::function<void()> wake);
    ~X11Clipboard();
    X11Clipboard(const X11Clipboard&) = delete;
    X11Clipboard& operator=(const X11Clipboard&) = delete;

    bool available() const { return available_; }  // false: no X display / no XFixes
    std::vector<ClipboardData> takeCaptured();

    void setCapturing(bool on) { capturing_ = on; }
    void setCaptureImages(bool on) { captureImages_ = on; }

    // put data on the clipboard. primaryToo: the PRIMARY selection as well (what terminals paste
    // with Shift+Insert). blocks until the X server confirms we own it, ~1 ms
    bool offer(ClipboardData data, bool primaryToo);

private:
    struct Impl;
    struct Command {
        std::shared_ptr<const ClipboardData> data;
        bool primaryToo = false;
        std::promise<bool> done;
    };

    void run();
    void wakeThread();

    std::function<void()> wake_;
    int wakeFd_ = -1;  // eventfd, pokes the x thread out of poll()
    std::atomic<bool> stop_{false};
    std::atomic<bool> available_{false};
    std::atomic<bool> capturing_{true};
    std::atomic<bool> captureImages_{true};

    std::mutex mutex_;
    std::vector<ClipboardData> captured_;
    std::vector<std::unique_ptr<Command>> commands_;
    std::thread thread_;
};

}  // namespace clipper
