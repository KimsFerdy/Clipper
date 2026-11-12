// the windows clipboard. a message-only window on its own thread:
// - AddClipboardFormatListener says when something got copied (no polling). we wait a moment (apps
//   often put several formats in a row), open the clipboard, copy what we want out and close it again
// - what we read, in order: files (CF_HDROP + Preferred DropEffect for cut), text (CF_UNICODETEXT),
//   images ("PNG" if the app gives one, CF_DIBV5 / CF_DIB otherwise, turned into png here)
// - copies password managers mark as secret are skipped: ExcludeClipboardContentFromMonitorProcessing,
//   Clipboard Viewer Ignore, CanIncludeInClipboardHistory = 0 (KeePassXC and KeePass set these)
// - offering puts everything on the clipboard right away (no delayed rendering), so it stays pasteable
//   after we quit, and the sequence number it gets tells the listener to skip our own copy

#include "platform/Win32Util.h"  // windows.h, before the other windows headers

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#include "core/FileList.h"
#include "core/Png.h"
#include "platform/Clipboard.h"

// own static copies, the app's Thumbnails.cpp has the shared one
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace clipper {

namespace {

constexpr size_t kMaxText = 16u << 20;    // utf-8 bytes, same as linux
constexpr size_t kMaxImage = 64u << 20;   // png bytes
constexpr size_t kMaxDib = 512u << 20;    // raw pixels before encoding, an 8k screenshot is ~130 MB
constexpr UINT kMsgCommands = WM_APP + 1;
constexpr UINT_PTR kSettleTimer = 1;
constexpr UINT kSettleMs = 60;
constexpr int kOpenRetries = 10;

bool isBlank(const std::string& s) {
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; });
}

// GlobalSize can be rounded up, cut the png after its IEND chunk
std::string trimPng(std::string png) {
    const size_t iend = png.rfind("IEND");
    if (iend != std::string::npos && iend + 8 <= png.size()) png.resize(iend + 8);
    return png;
}

std::string encodePng(const std::vector<uint8_t>& rgba, int w, int h) {
    std::string out;
    stbi_write_png_to_func([](void* ctx, void* data, int size) {
        static_cast<std::string*>(ctx)->append(static_cast<const char*>(data), static_cast<size_t>(size));
    }, &out, w, h, 4, rgba.data(), w * 4);
    return out;
}

// value of one channel of a 32 bit pixel, scaled to 0..255
uint8_t channel(uint32_t px, uint32_t mask) {
    if (!mask) return 0;
    unsigned shift = 0;
    while (!((mask >> shift) & 1)) ++shift;
    const uint32_t m = mask >> shift;
    const uint32_t v = (px & mask) >> shift;
    return static_cast<uint8_t>(m == 0xFF ? v : v * 255 / m);
}

// CF_DIB / CF_DIBV5 bytes -> rgba, top row first
bool dibToRgba(const std::string& dib, int* width, int* height, std::vector<uint8_t>* rgba) {
    if (dib.size() < sizeof(BITMAPINFOHEADER)) return false;
    BITMAPINFOHEADER bih;
    std::memcpy(&bih, dib.data(), sizeof bih);
    const int w = bih.biWidth, h = bih.biHeight < 0 ? -bih.biHeight : bih.biHeight;
    if (bih.biSize < sizeof bih || w <= 0 || h <= 0 || w > 32768 || h > 32768) return false;
    const bool topDown = bih.biHeight < 0;
    const bool fields = bih.biCompression == BI_BITFIELDS;
    const size_t palette = bih.biClrUsed ? bih.biClrUsed : (bih.biBitCount <= 8 ? (1u << bih.biBitCount) : 0);
    const size_t offset = bih.biSize + (fields && bih.biSize == sizeof bih ? 12 : 0) + palette * 4;
    *width = w;
    *height = h;
    rgba->assign(static_cast<size_t>(w) * h * 4, 0);

    if (bih.biBitCount == 32 && (bih.biCompression == BI_RGB || fields)) {
        uint32_t masks[4] = {0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000};  // r g b a
        if (fields) {
            const size_t at = sizeof bih;  // right after the v1 header, inside v4 / v5 ones
            const size_t n = bih.biSize >= sizeof(BITMAPV4HEADER) ? 4 : 3;
            if (dib.size() < at + n * 4) return false;
            std::memcpy(masks, dib.data() + at, n * 4);
            if (n == 3) masks[3] = 0;
        }
        const size_t stride = static_cast<size_t>(w) * 4;
        if (dib.size() < offset + stride * h) return false;
        bool anyAlpha = false;
        for (int y = 0; y < h; ++y) {
            const uint8_t* row = reinterpret_cast<const uint8_t*>(dib.data()) + offset + stride * (topDown ? y : h - 1 - y);
            uint8_t* out = rgba->data() + static_cast<size_t>(y) * w * 4;
            for (int x = 0; x < w; ++x) {
                uint32_t px;
                std::memcpy(&px, row + x * 4, 4);
                out[x * 4 + 0] = channel(px, masks[0]);
                out[x * 4 + 1] = channel(px, masks[1]);
                out[x * 4 + 2] = channel(px, masks[2]);
                out[x * 4 + 3] = channel(px, masks[3]);
                anyAlpha |= out[x * 4 + 3] != 0;
            }
        }
        // 32 bit without real alpha (print screen, most apps) has 0 there, that's opaque
        if (!anyAlpha) {
            for (size_t i = 3; i < rgba->size(); i += 4) (*rgba)[i] = 255;
        }
        return true;
    }

    // everything else (24 bit, palettes, 16 bit, rle): let gdi draw it into a 32 bit top-down bitmap
    if (offset >= dib.size()) return false;
    BITMAPINFO out{};
    out.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    out.bmiHeader.biWidth = w;
    out.bmiHeader.biHeight = -h;
    out.bmiHeader.biPlanes = 1;
    out.bmiHeader.biBitCount = 32;
    out.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP section = CreateDIBSection(screen, &out, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC mem = CreateCompatibleDC(screen);
    bool ok = false;
    if (section && mem) {
        HGDIOBJ old = SelectObject(mem, section);
        ok = SetDIBitsToDevice(mem, 0, 0, static_cast<DWORD>(w), static_cast<DWORD>(h), 0, 0, 0, static_cast<UINT>(h),
                               dib.data() + offset, reinterpret_cast<const BITMAPINFO*>(dib.data()), DIB_RGB_COLORS) > 0;
        GdiFlush();
        if (ok) {
            const uint8_t* src = static_cast<const uint8_t*>(bits);
            for (size_t i = 0; i < rgba->size(); i += 4) {
                (*rgba)[i + 0] = src[i + 2];
                (*rgba)[i + 1] = src[i + 1];
                (*rgba)[i + 2] = src[i + 0];
                (*rgba)[i + 3] = 255;
            }
        }
        SelectObject(mem, old);
    }
    if (mem) DeleteDC(mem);
    if (section) DeleteObject(section);
    ReleaseDC(nullptr, screen);
    return ok;
}

HGLOBAL globalCopy(const void* data, size_t size) {
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, std::max<size_t>(size, 1));
    if (!g) return nullptr;
    if (void* p = GlobalLock(g)) {
        if (size) std::memcpy(p, data, size);
        GlobalUnlock(g);
        return g;
    }
    GlobalFree(g);
    return nullptr;
}

HGLOBAL globalText(const std::string& utf8) {
    const std::wstring w = win32::widen(utf8);
    return globalCopy(w.c_str(), (w.size() + 1) * sizeof(wchar_t));
}

// png -> CF_DIB, 32 bit bottom-up, flattened onto white for apps that don't know alpha (paint)
HGLOBAL globalDib(const std::string& png) {
    int w = 0, h = 0, channels = 0;
    uint8_t* rgba = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(png.data()), static_cast<int>(png.size()), &w,
                                          &h, &channels, 4);
    if (!rgba) return nullptr;
    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof bih;
    bih.biWidth = w;
    bih.biHeight = h;
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;
    bih.biSizeImage = static_cast<DWORD>(w) * h * 4;
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, sizeof bih + bih.biSizeImage);
    uint8_t* p = g ? static_cast<uint8_t*>(GlobalLock(g)) : nullptr;
    if (p) {
        std::memcpy(p, &bih, sizeof bih);
        uint8_t* px = p + sizeof bih;
        for (int y = 0; y < h; ++y) {
            const uint8_t* src = rgba + static_cast<size_t>(h - 1 - y) * w * 4;
            uint8_t* dst = px + static_cast<size_t>(y) * w * 4;
            for (int x = 0; x < w; ++x) {
                const unsigned a = src[x * 4 + 3];
                for (int c = 0; c < 3; ++c)
                    dst[x * 4 + (2 - c)] = static_cast<uint8_t>((src[x * 4 + c] * a + 255 * (255 - a)) / 255);
                dst[x * 4 + 3] = 255;
            }
        }
        GlobalUnlock(g);
    } else if (g) {
        GlobalFree(g);
        g = nullptr;
    }
    stbi_image_free(rgba);
    return g;
}

HGLOBAL globalDropFiles(const std::vector<std::wstring>& paths) {
    std::wstring list;
    for (const std::wstring& p : paths) list += p + L'\0';
    list += L'\0';
    const size_t size = sizeof(DROPFILES) + list.size() * sizeof(wchar_t);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
    auto* df = g ? static_cast<DROPFILES*>(GlobalLock(g)) : nullptr;
    if (!df) {
        if (g) GlobalFree(g);
        return nullptr;
    }
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    std::memcpy(df + 1, list.data(), list.size() * sizeof(wchar_t));
    GlobalUnlock(g);
    return g;
}

std::string globalBytes(UINT format, size_t limit) {
    HANDLE h = GetClipboardData(format);
    if (!h) return {};
    const size_t size = GlobalSize(h);
    if (size == 0 || size > limit) return {};
    const void* p = GlobalLock(h);
    if (!p) return {};
    std::string out(static_cast<const char*>(p), size);
    GlobalUnlock(h);
    return out;
}

}  // namespace

struct Clipboard::Impl {
    Clipboard& self;
    HWND hwnd = nullptr;
    UINT cfPng = RegisterClipboardFormatW(L"PNG");
    UINT cfDropEffect = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    UINT cfExclude = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    UINT cfIgnore = RegisterClipboardFormatW(L"Clipboard Viewer Ignore");
    UINT cfHistory = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    UINT cfKdeSecret = RegisterClipboardFormatW(L"x-kde-passwordManagerHint");
    DWORD seenSeq = 0;  // the copy we already looked at (or made ourselves)
    int retries = 0;

    // what came off the clipboard, copied out so it can be closed again quickly
    struct Raw {
        enum class Kind { None, Files, Text, Png, Dib } kind = Kind::None;
        std::vector<std::wstring> files;
        bool cut = false;
        std::wstring text;
        std::string bytes;
    };

    bool secret() const {
        if (IsClipboardFormatAvailable(cfExclude) || IsClipboardFormatAvailable(cfIgnore) ||
            IsClipboardFormatAvailable(cfKdeSecret))
            return true;
        if (IsClipboardFormatAvailable(cfHistory)) {
            const std::string v = globalBytes(cfHistory, 64);
            DWORD allowed = 1;
            if (v.size() >= sizeof allowed) std::memcpy(&allowed, v.data(), sizeof allowed);
            return allowed == 0;
        }
        return false;
    }

    Raw readOpen() const {
        Raw raw;
        if (secret()) return raw;
        if (IsClipboardFormatAvailable(CF_HDROP)) {
            if (auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP))) {
                const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                for (UINT i = 0; i < n; ++i) {
                    std::wstring path(DragQueryFileW(drop, i, nullptr, 0) + 1, L'\0');
                    path.resize(DragQueryFileW(drop, i, path.data(), static_cast<UINT>(path.size())));
                    if (!path.empty()) raw.files.push_back(std::move(path));
                }
                if (!raw.files.empty()) {
                    raw.kind = Raw::Kind::Files;
                    const std::string effect = globalBytes(cfDropEffect, 64);
                    DWORD e = DROPEFFECT_COPY;
                    if (effect.size() >= sizeof e) std::memcpy(&e, effect.data(), sizeof e);
                    raw.cut = (e & DROPEFFECT_MOVE) != 0;
                    return raw;
                }
            }
        }
        // text beats images, office suites offer both and mean the text
        if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
            const std::string bytes = globalBytes(CF_UNICODETEXT, kMaxText * 2);
            const auto* w = reinterpret_cast<const wchar_t*>(bytes.data());
            raw.text.assign(w, wcsnlen(w, bytes.size() / sizeof(wchar_t)));
            if (!raw.text.empty()) {
                raw.kind = Raw::Kind::Text;
                return raw;
            }
        }
        if (!self.captureImages_) return raw;
        if (IsClipboardFormatAvailable(cfPng)) {
            raw.bytes = trimPng(globalBytes(cfPng, kMaxImage + 4096));
            uint32_t w, h;
            if (pngSize(raw.bytes, &w, &h)) {
                raw.kind = Raw::Kind::Png;
                return raw;
            }
        }
        for (UINT format : {static_cast<UINT>(CF_DIBV5), static_cast<UINT>(CF_DIB)}) {
            if (!IsClipboardFormatAvailable(format)) continue;
            raw.bytes = globalBytes(format, kMaxDib);
            if (!raw.bytes.empty()) {
                raw.kind = Raw::Kind::Dib;
                return raw;
            }
        }
        return raw;
    }

    void capture() {
        if (!self.capturing_) return;
        const DWORD seq = GetClipboardSequenceNumber();
        if (seq == seenSeq) return;
        if (GetClipboardOwner() == hwnd) {
            seenSeq = seq;
            return;
        }
        if (!OpenClipboard(hwnd)) {  // the app that copied may still have it open
            if (++retries < kOpenRetries) {
                SetTimer(hwnd, kSettleTimer, kSettleMs, nullptr);
            } else {
                retries = 0;
                seenSeq = seq;
            }
            return;
        }
        retries = 0;
        seenSeq = seq;
        HWND owner = GetClipboardOwner();
        Raw raw = readOpen();
        CloseClipboard();
        if (raw.kind == Raw::Kind::None) return;

        ClipboardData d;
        d.source = win32::windowProcessName(owner);
        if (d.source.empty()) d.source = win32::windowProcessName(GetForegroundWindow());
        switch (raw.kind) {
            case Raw::Kind::Files:
                d.kind = ClipKind::Files;
                d.cut = raw.cut;
                for (const std::wstring& f : raw.files) d.text += (d.text.empty() ? "" : "\n") + pathToUri(win32::narrow(f));
                break;
            case Raw::Kind::Text:
                d.kind = ClipKind::Text;
                d.text = win32::narrow(raw.text);
                if (d.text.size() > kMaxText || isBlank(d.text)) return;
                break;
            case Raw::Kind::Png:
                d.kind = ClipKind::Image;
                d.png = std::move(raw.bytes);
                if (d.png.size() > kMaxImage) return;
                break;
            case Raw::Kind::Dib: {
                d.kind = ClipKind::Image;
                int w = 0, h = 0;
                std::vector<uint8_t> rgba;
                if (!dibToRgba(raw.bytes, &w, &h, &rgba)) return;
                raw.bytes = {};
                d.png = encodePng(rgba, w, h);
                if (d.png.empty() || d.png.size() > kMaxImage) return;
                break;
            }
            case Raw::Kind::None:
                return;
        }
        {
            std::lock_guard lock(self.mutex_);
            self.captured_.push_back(std::move(d));
        }
        if (self.wake_) self.wake_();
    }

    bool own(const ClipboardData& d) {
        // everything gets built before opening, the clipboard stays locked for as short as possible
        std::vector<std::pair<UINT, HGLOBAL>> items;
        switch (d.kind) {
            case ClipKind::Text:
                items.emplace_back(CF_UNICODETEXT, globalText(d.text));
                break;
            case ClipKind::Image:
                items.emplace_back(cfPng, globalCopy(d.png.data(), d.png.size()));
                items.emplace_back(CF_DIB, globalDib(d.png));
                break;
            case ClipKind::Files: {
                std::vector<std::wstring> paths;
                std::string text;
                for (const std::string& uri : uriLines(d.text)) {
                    const std::string path = uriToPath(uri);
                    if (path.empty()) continue;
                    paths.push_back(win32::widen(path));
                    text += (text.empty() ? "" : "\r\n") + path;
                }
                if (paths.empty()) return false;
                const DWORD effect = d.cut ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
                items.emplace_back(CF_HDROP, globalDropFiles(paths));
                items.emplace_back(cfDropEffect, globalCopy(&effect, sizeof effect));
                items.emplace_back(CF_UNICODETEXT, globalText(text));
                break;
            }
        }
        auto freeAll = [&] {
            for (auto& [format, g] : items)
                if (g) GlobalFree(g);
        };
        if (items.empty() || !items[0].second) {
            freeAll();
            return false;
        }
        bool open = false;
        for (int i = 0; i < kOpenRetries && !(open = OpenClipboard(hwnd)); ++i) Sleep(15);
        if (!open) {
            freeAll();
            return false;
        }
        EmptyClipboard();
        bool ok = true;
        for (auto& [format, g] : items) {
            if (!g) continue;
            if (SetClipboardData(format, g)) {
                g = nullptr;  // the system owns it now
            } else if (format == items[0].first) {
                ok = false;
            }
        }
        CloseClipboard();
        freeAll();
        seenSeq = GetClipboardSequenceNumber();  // don't capture what we just put there
        return ok;
    }

    void runCommands() {
        std::vector<std::unique_ptr<Command>> cmds;
        {
            std::lock_guard lock(self.mutex_);
            cmds.swap(self.commands_);
        }
        for (auto& c : cmds) c->done.set_value(own(*c->data));
    }

    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        }
        auto* impl = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!impl) return DefWindowProcW(hwnd, msg, wp, lp);
        switch (msg) {
            case WM_CLIPBOARDUPDATE:
                // settle first: apps put formats on one after the other, and still hold it for a moment
                SetTimer(hwnd, kSettleTimer, kSettleMs, nullptr);
                return 0;
            case WM_TIMER:
                if (wp == kSettleTimer) {
                    KillTimer(hwnd, kSettleTimer);
                    impl->capture();
                }
                return 0;
            case kMsgCommands:
                impl->runCommands();
                return 0;
            case WM_CLOSE:
                DestroyWindow(hwnd);
                return 0;
            case WM_DESTROY:
                RemoveClipboardFormatListener(hwnd);
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
};

Clipboard::Clipboard(std::function<void()> wake) : wake_(std::move(wake)) {
    auto ready = std::make_shared<std::promise<void>>();  // shared: set_value may still run as we return
    std::future<void> started = ready->get_future();
    thread_ = std::thread([this, ready] {
        Impl impl{*this};
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = Impl::proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"ClipperClipboard";
        RegisterClassExW(&wc);
        impl.hwnd = CreateWindowExW(0, wc.lpszClassName, L"Clipper clipboard", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                    wc.hInstance, &impl);
        if (impl.hwnd && AddClipboardFormatListener(impl.hwnd)) {
            impl.seenSeq = GetClipboardSequenceNumber();  // what's there from before we started isn't a new copy
            hwnd_ = impl.hwnd;
            available_ = true;
        }
        ready->set_value();
        if (!available_) {
            if (impl.hwnd) DestroyWindow(impl.hwnd);
            return;
        }
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
        hwnd_ = nullptr;
        // offers that came in while shutting down
        std::lock_guard lock(mutex_);
        for (auto& c : commands_) c->done.set_value(false);
        commands_.clear();
    });
    started.wait();
}

Clipboard::~Clipboard() {
    stop_ = true;
    if (HWND hwnd = static_cast<HWND>(hwnd_.load())) PostMessageW(hwnd, WM_CLOSE, 0, 0);
    if (thread_.joinable()) thread_.join();
}

std::vector<ClipboardData> Clipboard::takeCaptured() {
    std::lock_guard lock(mutex_);
    return std::exchange(captured_, {});
}

void Clipboard::wakeThread() {
    if (HWND hwnd = static_cast<HWND>(hwnd_.load())) PostMessageW(hwnd, kMsgCommands, 0, 0);
}

bool Clipboard::offer(ClipboardData data, bool /*primaryToo: no PRIMARY on windows*/) {
    if (!hwnd_.load()) return false;
    auto cmd = std::make_unique<Command>();
    cmd->data = std::make_shared<const ClipboardData>(std::move(data));
    std::future<bool> done = cmd->done.get_future();
    {
        std::lock_guard lock(mutex_);
        commands_.push_back(std::move(cmd));
    }
    wakeThread();
    // a big image takes a moment to turn into a dib, a stuck clipboard (someone else holding it) gives up
    if (done.wait_for(std::chrono::seconds(3)) != std::future_status::ready) return false;
    return done.get();
}

}  // namespace clipper
