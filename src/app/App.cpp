#include "app/App.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <thread>
#include <utility>

#include "AppIcon.h"
#include "app/Theme.h"
#include "core/FileList.h"
#include "imgui.h"
#include "imgui_internal.h"  // RenderTextEllipsis
#include "misc/cpp/imgui_stdlib.h"
#include "platform/Autostart.h"
#include "platform/GlobalShortcut.h"
#include "platform/Paths.h"
#include "platform/Shell.h"
#include "stb_image.h"

#ifndef CLIPPER_VERSION
#define CLIPPER_VERSION "dev"
#endif

namespace clipper {

namespace {

constexpr size_t kMaxEditableText = 256 * 1024;  // imgui's multiline input walks the whole buffer every frame

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::tm localTime(std::time_t t) {
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

std::string formatSize(uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 3) {
        v /= 1024.0;
        ++u;
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, u == 0 ? "%.0f %s" : "%.1f %s", v, units[u]);
    return buf;
}

std::string formatAge(int64_t ms) {
    const int64_t s = std::max<int64_t>(0, (nowMs() - ms) / 1000);
    char buf[48];
    if (s < 60)
        return "now";
    else if (s < 3600)
        std::snprintf(buf, sizeof buf, "%lld min", static_cast<long long>(s / 60));
    else if (s < 86400)
        std::snprintf(buf, sizeof buf, "%lld h", static_cast<long long>(s / 3600));
    else if (s < 7 * 86400)
        std::snprintf(buf, sizeof buf, "%lld d", static_cast<long long>(s / 86400));
    else {
        std::time_t t = static_cast<std::time_t>(ms / 1000);
        const std::tm tm = localTime(t);
        std::strftime(buf, sizeof buf, "%b %d", &tm);
    }
    return buf;
}

std::string formatTime(int64_t ms) {
    std::time_t t = static_cast<std::time_t>(ms / 1000);
    const std::tm tm = localTime(t);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
    return buf;
}

std::string toLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

ImU32 withAlpha(ImU32 c, float a) {
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    v.w *= a;
    return ImGui::ColorConvertFloat4ToU32(v);
}

// app icon as ARGB32 (network order) at a few sizes, the panel picks one
std::vector<Tray::Icon> trayIcons() {
    int w, h, channels;
    uint8_t* rgba = stbi_load_from_memory(kAppIconPng, sizeof kAppIconPng, &w, &h, &channels, 4);
    if (!rgba) return {};
    std::vector<Tray::Icon> icons;
    for (int size : {w, 64, 32}) {
        const int step = w / size;  // 256 -> 64/32 is an exact box filter
        if (step < 1 || w % size != 0 || h != w) continue;
        Tray::Icon icon{size, size, std::vector<uint8_t>(static_cast<size_t>(size) * size * 4)};
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                unsigned sum[4] = {0, 0, 0, 0};
                for (int dy = 0; dy < step; ++dy) {
                    for (int dx = 0; dx < step; ++dx) {
                        const uint8_t* p = rgba + ((y * step + dy) * w + (x * step + dx)) * 4;
                        for (int c = 0; c < 4; ++c) sum[c] += p[c];
                    }
                }
                uint8_t* out = &icon.argb[(static_cast<size_t>(y) * size + x) * 4];
                const unsigned n = static_cast<unsigned>(step * step);
                out[0] = static_cast<uint8_t>(sum[3] / n);  // A R G B
                out[1] = static_cast<uint8_t>(sum[0] / n);
                out[2] = static_cast<uint8_t>(sum[1] / n);
                out[3] = static_cast<uint8_t>(sum[2] / n);
            }
        }
        icons.push_back(std::move(icon));
    }
    stbi_image_free(rgba);
    return icons;
}

// one line of text squeezed into [x0, x1] with "..." if it doesn't fit
void clippedText(ImVec2 pos, float x1, const std::string& text, ImU32 color) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(ImVec2(pos.x, pos.y), ImVec2(x1, pos.y + ImGui::GetTextLineHeight() + 2), true);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::RenderTextEllipsis(dl, pos, ImVec2(x1, pos.y + ImGui::GetTextLineHeight()), x1, x1, text.c_str(),
                              text.c_str() + text.size(), nullptr);
    ImGui::PopStyleColor();
    dl->PopClipRect();
}

void hint(const char* text) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(themeColors().dimText, "%s", text);
    ImGui::PopTextWrapPos();
}

// ---- the drawn bits of the look ----

float strokeWidth() { return std::max(1.0f, std::round(ImGui::GetFontSize() / 11.0f)); }

// list rows: a soft raised pill (hairline + a whisper of shadow) when selected, a faint one when hovered.
// call right after the Selectable, with the Header colours pushed transparent (RowColors below)
void rowBackground(bool selected, bool hovered) {
    if (!selected && !hovered) return;
    const ThemeColors& tc = themeColors();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const float r = ImGui::GetStyle().FrameRounding;
    if (selected) {
        dl->AddRectFilled(ImVec2(a.x, a.y + 1), ImVec2(b.x, b.y + 1.5f), IM_COL32(0, 0, 0, tc.dark ? 60 : 14), r);
        dl->AddRectFilled(a, b, tc.raised, r);
        dl->AddRect(a, b, tc.hairline, r, 0, 1.0f);
    } else {
        dl->AddRectFilled(a, b, tc.hover, r);
    }
}

// selectables draw nothing themselves, rowBackground does it rounded. pop in the same window (before EndChild)
void pushRowColors() {
    ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(0, 0, 0, 0));
}
void popRowColors() { ImGui::PopStyleColor(3); }

// "12" in a little rounded badge, right edge at x
float countPill(float right, float centerY, size_t count) {
    const ThemeColors& tc = themeColors();
    const std::string n = std::to_string(count);
    const ImVec2 ts = ImGui::CalcTextSize(n.c_str());
    const float h = std::round(ImGui::GetTextLineHeight() * 1.1f);
    const float w = std::max(h, ts.x + h * 0.7f);
    const ImVec2 a(right - w, std::round(centerY - h * 0.5f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, ImVec2(right, a.y + h), tc.pill, h * 0.5f);
    dl->AddText(ImVec2(std::round(a.x + (w - ts.x) * 0.5f), std::round(a.y + (h - ts.y) * 0.5f)),
                ImGui::GetColorU32(tc.dimText), n.c_str());
    return w;
}

void iconClock(ImVec2 c, float r, ImU32 col) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float t = strokeWidth();
    dl->AddCircle(c, r, col, 24, t);
    dl->AddLine(c, ImVec2(c.x, c.y - r * 0.55f), col, t);
    dl->AddLine(c, ImVec2(c.x + r * 0.4f, c.y + r * 0.2f), col, t);
}

// pinned = a bookmark ribbon. filled on pinned rows, outline in the sidebar
void iconPin(ImVec2 c, float r, ImU32 col, bool filled = false) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float x0 = std::round(c.x - r * 0.62f), x1 = std::round(c.x + r * 0.62f);
    const float y0 = std::round(c.y - r * 0.85f), y1 = std::round(c.y + r * 0.85f);
    const ImVec2 pts[] = {ImVec2(x0, y0), ImVec2(x1, y0), ImVec2(x1, y1), ImVec2(c.x, y1 - r * 0.5f), ImVec2(x0, y1)};
    if (filled)
        dl->AddConcavePolyFilled(pts, 5, col);
    else
        dl->AddPolyline(pts, 5, col, ImDrawFlags_Closed, strokeWidth());
}

void iconFolder(ImVec2 c, float r, ImU32 col) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float x0 = c.x - r, x1 = c.x + r, y0 = c.y - r * 0.72f, y1 = c.y + r * 0.72f;
    const ImVec2 pts[] = {ImVec2(x0, y1),           ImVec2(x0, y0),           ImVec2(x0 + r * 0.75f, y0),
                          ImVec2(x0 + r, y0 + r * 0.28f), ImVec2(x1, y0 + r * 0.28f), ImVec2(x1, y1)};
    dl->AddPolyline(pts, 6, col, ImDrawFlags_Closed, strokeWidth());
}

void iconSearch(ImVec2 c, float r, ImU32 col) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float t = strokeWidth();
    const ImVec2 lens(c.x - r * 0.2f, c.y - r * 0.2f);
    dl->AddCircle(lens, r * 0.7f, col, 20, t);
    dl->AddLine(ImVec2(lens.x + r * 0.5f, lens.y + r * 0.5f), ImVec2(c.x + r * 0.85f, c.y + r * 0.85f), col, t);
}

// rounded input with a magnifier in front. GetItemID() after it is the input's
bool searchField(const char* id, const char* hintText, std::string* text, float width, ImGuiInputTextFlags flags = 0) {
    const float em = ImGui::GetFontSize();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(std::round(em * 2.0f), ImGui::GetStyle().FramePadding.y));
    ImGui::SetNextItemWidth(width);
    const bool changed = ImGui::InputTextWithHint(id, hintText, text, flags);
    ImGui::PopStyleVar();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    iconSearch(ImVec2(a.x + em * 1.0f, (a.y + b.y) * 0.5f), em * 0.42f, ImGui::GetColorU32(themeColors().dimText));
    return changed;
}

// [ History | Pinned ] in a track, the picked one raised. true when it changed
bool segmented(const char* id, std::initializer_list<const char*> labels, int* value) {
    const ThemeColors& tc = themeColors();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float h = ImGui::GetFrameHeight(), pad = std::round(style.FramePadding.x * 0.3f);
    float w = pad * 2;
    for (const char* l : labels) w += ImGui::CalcTextSize(l).x + style.FramePadding.x * 2;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), tc.pill, style.FrameRounding);
    bool changed = false;
    float x = p.x + pad;
    int i = 0;
    ImGui::PushID(id);
    for (const char* l : labels) {
        const float lw = ImGui::CalcTextSize(l).x + style.FramePadding.x * 2;
        ImGui::SetCursorScreenPos(ImVec2(x, p.y + pad));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("seg", ImVec2(lw, h - pad * 2)) && *value != i) {
            *value = i;
            changed = true;
        }
        ImGui::PopID();
        const bool on = *value == i;
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        if (on) {
            dl->AddRectFilled(a, b, tc.raised, style.FrameRounding - pad);
            dl->AddRect(a, b, tc.hairline, style.FrameRounding - pad);
        }
        const ImVec2 ts = ImGui::CalcTextSize(l);
        dl->AddText(ImVec2(std::round(a.x + (lw - ts.x) * 0.5f), std::round((a.y + b.y - ts.y) * 0.5f)),
                    on || ImGui::IsItemHovered() ? ImGui::GetColorU32(ImGuiCol_Text) : ImGui::GetColorU32(tc.dimText), l);
        x += lw;
        ++i;
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(w, h));
    return changed;
}

// on / off switch, accent track when on. true when it flipped
bool toggleSwitch(const char* label, bool* on) {
    const ThemeColors& tc = themeColors();
    const float lineH = ImGui::GetTextLineHeight();
    const float h = std::round(lineH * 0.95f), w = std::round(h * 1.75f);
    const float frameH = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float labelW = ImGui::CalcTextSize(label, nullptr, true).x;
    const float gap = ImGui::GetStyle().ItemInnerSpacing.x;
    const bool clicked = ImGui::InvisibleButton(label, ImVec2(w + gap + labelW, frameH));
    if (clicked) *on = !*on;
    const bool hot = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a(p.x, std::round(p.y + (frameH - h) * 0.5f)), b(a.x + w, a.y + h);
    dl->AddRectFilled(a, b, *on ? tc.accent : (hot ? tc.hover : tc.pill), h * 0.5f);
    if (!*on) dl->AddRect(a, b, tc.hairline, h * 0.5f);
    const float r = h * 0.5f - 2.0f;
    const ImVec2 knob(*on ? b.x - h * 0.5f : a.x + h * 0.5f, a.y + h * 0.5f);
    dl->AddCircleFilled(ImVec2(knob.x, knob.y + 0.5f), r + 0.5f, IM_COL32(0, 0, 0, 40), 20);
    dl->AddCircleFilled(knob, r, IM_COL32(255, 255, 255, 255), 20);
    dl->AddText(ImVec2(b.x + gap, std::round(p.y + (frameH - lineH) * 0.5f)), ImGui::GetColorU32(ImGuiCol_Text), label,
                ImGui::FindRenderedTextEnd(label));
    return clicked;
}

// card = rounded panel with a hairline, for the list and the details
bool beginCard(const char* id, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, themeColors().panel);
    const bool open = ImGui::BeginChild(id, size, ImGuiChildFlags_Borders);
    ImGui::PopStyleColor();
    return open;
}

void sectionLabel(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, themeColors().faint);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

// view ids for drag and drop targets
constexpr int kViewHistory = 0, kViewPinned = 1, kViewFolder = 2;

}  // namespace

App::App(const SystemAppearance& system, float displayScale, std::function<void()> wake, Desktop& desktop)
    : wake_(std::move(wake)),
      desktop_(desktop),
      settings_(loadSettings()),
      systemAppearance_(system),
      displayScale_(displayScale),
      store_(dataDirectory()) {
    applyTheme(appearance(), displayScale_);
    if (!store_.load(&storeError_)) std::fprintf(stderr, "clipper: %s\n", storeError_.c_str());
    thumbs_ = std::make_unique<Thumbnails>(wake_);
    clipboard_ = std::make_unique<Clipboard>(wake_);
    clipboard_->setCapturing(settings_.capturing);
    clipboard_->setCaptureImages(settings_.captureImages);
    updateTray();
}

App::~App() = default;

void App::updateTray() {
    if (settings_.trayIcon && !tray_) tray_ = std::make_unique<Tray>(trayIcons(), wake_);
    if (!settings_.trayIcon) tray_.reset();
    if (tray_) tray_->setPaused(!settings_.capturing);
}

void App::start(bool background) {
    // first run: the shortcut and starting at login are what make it a clipboard manager, so set them up.
    // both can be turned off in Preferences and stay off after that
    if (settings_.firstRun) {
        settings_.firstRun = false;
        if (!settings_.launchAtLogin) settings_.launchAtLogin = setLaunchAtLogin(true);
        if (settings_.popupShortcut && !setGlobalShortcut(settings_.shortcut)) settings_.popupShortcut = false;
        saveSettings(settings_);
        if (settings_.popupShortcut)
            setStatus("Press " + prettyAccel(settings_.shortcut) + " in any app to open your clipboard history");
    } else if (settings_.popupShortcut && shortcutDesktop() == ShortcutDesktop::Windows) {
        // RegisterHotKey only lasts as long as we run, so it gets set up on every start, not just the first
        if (!setGlobalShortcut(settings_.shortcut))
            setStatus(prettyAccel(settings_.shortcut) + " is taken by another app, pick another shortcut in Preferences");
    }

    startTime_ = std::chrono::steady_clock::now();
    if (background && tray_)
        waitingForTray_ = true;  // stay hidden, keep capturing
    else if (!background)
        windowRequest_ = WindowRequest::Manager;
    refilter();
}

void App::remote(const AppBus::Request& request) {
    waitingForTray_ = false;
    switch (request.kind) {
        case AppBus::Request::Kind::Popup:
            openPopup();
            break;
        case AppBus::Request::Kind::Toggle:
            windowRequest_ = WindowRequest::ToggleManager;
            break;
        case AppBus::Request::Kind::Show:
            windowRequest_ = WindowRequest::Manager;
            break;
        case AppBus::Request::Kind::Quit:
            quit_ = true;
            break;
    }
}

App::WindowRequest App::takeWindowRequest() { return std::exchange(windowRequest_, WindowRequest::None); }

bool App::popupAnchor(int* x, int* y) const {
    *x = anchorX_;
    *y = anchorY_;
    return haveAnchor_;
}

bool App::hideOnClose() const {
    return settings_.trayIcon && settings_.closeToTray && tray_ && tray_->available();
}

bool App::consumeFontRebuild() { return std::exchange(fontsDirty_, false); }

void App::rememberWindowGeometry(const WindowGeometry& geometry) {
    if (geometry == settings_.window) return;
    settings_.window = geometry;
    saveSettings(settings_);
}

SystemAppearance App::appearance() const {
    SystemAppearance a = systemAppearance_;
    if (settings_.theme == Settings::Theme::Dark) a.dark = true;
    if (settings_.theme == Settings::Theme::Light) a.dark = false;
    if (settings_.customAccent) {
        a.hasAccent = true;
        std::copy(std::begin(settings_.accent), std::end(settings_.accent), a.accent);
    }
    return a;
}

void App::setStatus(std::string message) {
    statusMessage_ = std::move(message);
    statusTime_ = std::chrono::steady_clock::now();
}

void App::setZoom(float zoom) {
    zoom = std::clamp(zoom, 0.7f, 2.0f);
    if (zoom == settings_.fontZoom) return;
    settings_.fontZoom = zoom;
    fontsDirty_ = true;
    saveSettings(settings_);
}

// ---- capture ----

bool App::excludedApp(const std::string& source) const {
    if (source.empty()) return false;
    const std::string s = toLower(source);
    return std::any_of(settings_.excludedApps.begin(), settings_.excludedApps.end(),
                       [&](const std::string& a) { return toLower(a) == s; });
}

void App::takeCaptures() {
    bool added = false;
    for (ClipboardData& d : clipboard_->takeCaptured()) {
        if (!settings_.capturing || excludedApp(d.source)) continue;
        NewClip c;
        c.kind = d.kind;
        c.text = std::move(d.text);
        c.png = std::move(d.png);
        c.cut = d.cut;
        c.source = std::move(d.source);
        if (store_.add(std::move(c), nowMs())) added = true;
    }
    if (added) store_.prune(static_cast<size_t>(settings_.maxClips));
}

void App::setCapturing(bool on) {
    if (settings_.capturing == on) return;
    settings_.capturing = on;
    clipboard_->setCapturing(on);
    if (tray_) tray_->setPaused(!on);
    saveSettings(settings_);
    setStatus(on ? "Capturing again" : "Paused, nothing you copy gets saved until you resume");
}

bool App::putOnClipboard(const Clip& clip, bool primaryToo) {
    ClipboardData d;
    d.kind = clip.kind;
    d.text = clip.text;
    // a cut that already got pasted once moved the files, cutting them again would fail. history = copies
    d.cut = false;
    if (clip.kind == ClipKind::Image && !store_.readImage(clip, &d.png)) return false;
    return clipboard_->offer(std::move(d), primaryToo);
}

void App::copyToClipboard(uint64_t id) {
    const Clip* c = store_.find(id);
    if (!c) return;
    if (!putOnClipboard(*c, false)) {
        setStatus("Could not take over the clipboard");
        return;
    }
    store_.touch(id, nowMs());
    setStatus("On the clipboard, Ctrl+V to paste it");
}

void App::pick(uint64_t id, bool paste) {
    const Clip* c = store_.find(id);
    if (!c) return;
    const bool canPaste = paste && settings_.autoPaste && target_ && target_ != ownWindow_ && desktop_.canSendKeys();
    const bool terminal = canPaste && desktop_.isTerminal(target_);
    if (!putOnClipboard(*c, terminal)) return;
    store_.touch(id, nowMs());
    pendingPaste_ = {canPaste, target_, terminal};
    windowRequest_ = WindowRequest::Hide;
}

void App::windowChanged(Mode mode) {
    const Mode before = mode_;
    mode_ = mode;
    if (mode == Mode::Popup) {
        popupHadFocus_ = false;
        popupFocusSearch_ = true;
    }
    if (mode == Mode::Manager && before != Mode::Manager) focusManagerSearch_ = true;
    if (mode == Mode::Hidden && pendingPaste_.active) pasteNow();
    pendingPaste_.active = false;
}

void App::pasteNow() {
    // back to the app the shortcut came from, then the paste key. the wm hands focus back on its own
    // most of the time, asking explicitly makes it reliable
    desktop_.activate(pendingPaste_.window);
    if (!desktop_.waitUntilActive(pendingPaste_.window, 500)) {
        // a Ctrl+V into whatever has focus instead would be worse than no paste. it's on the clipboard
        setStatus("Couldn't get back to the window, the clip is on the clipboard: paste it with Ctrl+V");
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(40));  // some apps (electron) ignore keys for a moment after getting focus
    desktop_.sendPaste(pendingPaste_.terminal);
}

void App::focusChanged(bool focused) {
    if (mode_ != Mode::Popup) return;
    if (focused) {
        popupHadFocus_ = true;
    } else if (popupHadFocus_ && windowRequest_ == WindowRequest::None && !pendingPaste_.active) {
        windowRequest_ = WindowRequest::Hide;  // clicked somewhere else: close like a menu would
    }
}

void App::deleteClip(uint64_t id) {
    if (!store_.find(id)) return;
    // keep the selection on the same row
    auto it = std::find(managerResults_.begin(), managerResults_.end(), id);
    uint64_t next = 0;
    if (it != managerResults_.end()) {
        if (it + 1 != managerResults_.end())
            next = *(it + 1);
        else if (it != managerResults_.begin())
            next = *(it - 1);
    }
    thumbs_->drop(id);
    store_.remove(id);
    if (selectedId_ == id) selectClip(next);
}

void App::applyPreferences(Settings next) {
    if (next.launchAtLogin != settings_.launchAtLogin && !setLaunchAtLogin(next.launchAtLogin)) {
        setStatus("Could not change the login startup entry");
        next.launchAtLogin = settings_.launchAtLogin;
    }
    const bool restyle = next.theme != settings_.theme || next.customAccent != settings_.customAccent ||
                         !std::equal(std::begin(next.accent), std::end(next.accent), std::begin(settings_.accent));
    const bool refont = restyle || next.fontZoom != settings_.fontZoom;

    const bool shortcutChanged =
        next.popupShortcut != settings_.popupShortcut || (next.popupShortcut && next.shortcut != settings_.shortcut);
    if (shortcutChanged) {
        if (!setGlobalShortcut(next.popupShortcut ? next.shortcut : "")) {
            setStatus("Could not change the shortcut");
            next.popupShortcut = settings_.popupShortcut;
            next.shortcut = settings_.shortcut;
        } else if (next.popupShortcut) {
            const std::string conflict = shortcutConflict(next.shortcut);
            setStatus(conflict.empty() ? prettyAccel(next.shortcut) + " now opens the Clipper popup"
                                       : prettyAccel(next.shortcut) + " is also used by \"" + conflict +
                                             "\", pick another one if it doesn't work");
        }
    }

    next.window = settings_.window;  // the dialog's copy is stale
    next.capturing = settings_.capturing;
    const bool smaller = next.maxClips < settings_.maxClips;
    settings_ = std::move(next);
    if (!saveSettings(settings_)) setStatus("Could not save settings to " + settingsFilePath());
    clipboard_->setCaptureImages(settings_.captureImages);
    updateTray();
    if (smaller) {
        const size_t dropped = store_.prune(static_cast<size_t>(settings_.maxClips));
        if (dropped) setStatus("Dropped the " + std::to_string(dropped) + " oldest clips");
    }
    if (restyle) applyTheme(appearance(), displayScale_);
    if (refont) fontsDirty_ = true;
}

// ---- loop ----

void App::tick() {
    takeCaptures();

    if (tray_) {
        for (Tray::Action action : tray_->takeActions()) {
            switch (action) {
                case Tray::Action::Toggle:
                    windowRequest_ = WindowRequest::ToggleManager;
                    break;
                case Tray::Action::Show:
                    windowRequest_ = WindowRequest::Manager;
                    break;
                case Tray::Action::TogglePause:
                    setCapturing(!settings_.capturing);
                    break;
                case Tray::Action::Quit:
                    quit_ = true;
                    break;
            }
        }
    }

    // started hidden but the panel never picked up our icon, don't stay invisible forever
    if (waitingForTray_) {
        if (tray_ && tray_->available()) {
            waitingForTray_ = false;
        } else if (std::chrono::steady_clock::now() - startTime_ > std::chrono::seconds(30)) {
            waitingForTray_ = false;
            windowRequest_ = WindowRequest::Manager;
        }
    }

    if (store_.revision() != seenRevision_) refilter();
}

void App::refilter() {
    seenRevision_ = store_.revision();
    popupResults_ = filterClips(store_, popupFilter_);
    popupRow_ = std::clamp(popupRow_, 0, std::max(0, static_cast<int>(popupResults_.size()) - 1));

    if (managerFilter_.view == ClipFilter::View::Folder && !store_.findFolder(managerFilter_.folder))
        managerFilter_.view = ClipFilter::View::History;
    managerResults_ = filterClips(store_, managerFilter_);
    if (!store_.find(selectedId_)) selectedId_ = 0;

    countAll_ = store_.clips().size();
    countPinned_ = 0;
    folderCounts_.assign(store_.folders().size(), 0);
    for (const Clip& c : store_.clips()) {
        countPinned_ += c.pinned;
        for (size_t i = 0; i < store_.folders().size(); ++i) folderCounts_[i] += c.folder == store_.folders()[i].id;
    }
}

const Clip* App::clipAt(const std::vector<uint64_t>& ids, int row) const {
    if (row < 0 || row >= static_cast<int>(ids.size())) return nullptr;
    return store_.find(ids[static_cast<size_t>(row)]);
}

void App::draw() {
    thumbs_->upload();
    if (mode_ == Mode::Popup)
        drawPopup();
    else
        drawManager();
    thumbs_->trim(200);
}

// ---- shared drawing ----

void App::drawKindIcon(const Clip& clip, ImVec2 o, float s, bool selected) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ThemeColors& tc = themeColors();
    const ImU32 color = selected ? tc.selectedText : tc.fileIcon;
    switch (clip.kind) {
        case ClipKind::Text: {  // page with lines
            const float w = s * 0.72f, x = o.x + (s - w) * 0.5f;
            dl->AddRect(ImVec2(x, o.y + s * 0.05f), ImVec2(x + w, o.y + s * 0.95f), color, s * 0.1f, 0, 1.2f);
            for (int i = 0; i < 3; ++i) {
                const float y = o.y + s * (0.32f + 0.2f * i);
                dl->AddLine(ImVec2(x + w * 0.22f, y), ImVec2(x + w * (i == 2 ? 0.55f : 0.78f), y), color, 1.0f);
            }
            break;
        }
        case ClipKind::Files: {  // folder
            const ImU32 c = selected ? tc.selectedText : tc.folderIcon;
            dl->AddRectFilled(ImVec2(o.x, o.y + s * 0.14f), ImVec2(o.x + s * 0.45f, o.y + s * 0.36f), c, s * 0.08f);
            dl->AddRectFilled(ImVec2(o.x, o.y + s * 0.26f), ImVec2(o.x + s, o.y + s * 0.86f), c, s * 0.1f);
            break;
        }
        case ClipKind::Image: {  // the picture itself if it's loaded, a mountain until then
            const Thumbnails::Texture tex = thumbs_->get(clip.id, store_.imagePath(clip));
            if (tex.id) {
                const float k = std::min(s / tex.width, s / tex.height);
                const ImVec2 sz(tex.width * k, tex.height * k);
                const ImVec2 p(o.x + (s - sz.x) * 0.5f, o.y + (s - sz.y) * 0.5f);
                dl->AddImageRounded(static_cast<ImTextureID>(tex.id), p, ImVec2(p.x + sz.x, p.y + sz.y), ImVec2(0, 0),
                                    ImVec2(1, 1), IM_COL32_WHITE, s * 0.08f);
            } else {
                dl->AddRect(ImVec2(o.x, o.y + s * 0.12f), ImVec2(o.x + s, o.y + s * 0.88f), color, s * 0.1f, 0, 1.2f);
                dl->AddTriangleFilled(ImVec2(o.x + s * 0.15f, o.y + s * 0.78f), ImVec2(o.x + s * 0.42f, o.y + s * 0.42f),
                                      ImVec2(o.x + s * 0.65f, o.y + s * 0.78f), color);
                dl->AddCircleFilled(ImVec2(o.x + s * 0.72f, o.y + s * 0.34f), s * 0.08f, color);
            }
            break;
        }
    }
}

void App::drawPreview(const Clip& clip, float height) {
    switch (clip.kind) {
        case ClipKind::Text: {
            ImGui::BeginChild("preview_text", ImVec2(0, height), ImGuiChildFlags_None);
            ImGui::PushTextWrapPos(0.0f);
            const size_t n = std::min<size_t>(clip.text.size(), 4000);
            ImGui::TextUnformatted(clip.text.data(), clip.text.data() + n);
            if (n < clip.text.size()) ImGui::TextColored(themeColors().dimText, "... %s in total", formatSize(clip.bytes).c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndChild();
            break;
        }
        case ClipKind::Image: {
            const Thumbnails::Texture tex = thumbs_->get(clip.id, store_.imagePath(clip));
            const ImVec2 avail(ImGui::GetContentRegionAvail().x, height);
            if (tex.id) {
                const float k = std::min({avail.x / tex.width, avail.y / tex.height, 2.0f});
                const ImVec2 sz(tex.width * k, tex.height * k);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail.x - sz.x) * 0.5f);
                ImGui::Image(static_cast<ImTextureID>(tex.id), sz);
                ImGui::Dummy(ImVec2(0, std::max(0.0f, avail.y - sz.y - ImGui::GetStyle().ItemSpacing.y)));
            } else {
                ImGui::Dummy(avail);
            }
            break;
        }
        case ClipKind::Files: {
            ImGui::BeginChild("preview_files", ImVec2(0, height), ImGuiChildFlags_None);
            std::error_code ec;
            for (const std::string& p : clipPaths(clip)) {
                const bool exists = std::filesystem::exists(p, ec);
                const bool dir = exists && std::filesystem::is_directory(p, ec);
                if (exists)
                    ImGui::Text("%s%s", p.c_str(), dir ? "/" : "");
                else
                    ImGui::TextColored(themeColors().dimText, "%s  (gone)", p.c_str());
            }
            ImGui::EndChild();
            break;
        }
    }
}

// ---- popup ----

void App::openPopup() {
    // who gets the paste: whatever had focus when the shortcut went off
    target_ = desktop_.activeWindow();
    if (target_ == ownWindow_) target_ = 0;
    haveAnchor_ = desktop_.pointer(&anchorX_, &anchorY_);
    popupFilter_.query.clear();
    popupFilter_.view = ClipFilter::View::History;
    popupRow_ = 0;
    refilter();
    windowRequest_ = WindowRequest::Popup;
}

void App::popupKeys() {
    const ImGuiIO& io = ImGui::GetIO();
    const int n = static_cast<int>(popupResults_.size());
    auto move = [&](int row) { popupRow_ = std::clamp(row, 0, std::max(0, n - 1)); };
    const int page = std::max(1, static_cast<int>(ImGui::GetWindowHeight() / (ImGui::GetTextLineHeight() * 2.2f)) - 1);

    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        windowRequest_ = WindowRequest::Hide;
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) move(popupRow_ + 1);
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) move(popupRow_ - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) move(popupRow_ + page);
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) move(popupRow_ - page);
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Home)) move(0);
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_End)) move(n - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
        popupFilter_.view =
            popupFilter_.view == ClipFilter::View::Pinned ? ClipFilter::View::History : ClipFilter::View::Pinned;
        popupRow_ = 0;
        refilter();
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_M, false)) {
        windowRequest_ = WindowRequest::Manager;
        return;
    }

    // Ctrl+1..9 = that row
    if (io.KeyCtrl) {
        for (int k = 0; k < 9; ++k) {
            if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_1 + k), false) && k < n) {
                pick(popupResults_[static_cast<size_t>(k)], true);
                return;
            }
        }
    }
    const Clip* c = clipAt(popupResults_, popupRow_);
    if (!c) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
        pick(c->id, !io.KeyCtrl);  // Ctrl+Enter: clipboard only
        return;
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_P, false)) store_.setPinned(c->id, !c->pinned);
    if (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
        thumbs_->drop(c->id);
        store_.remove(c->id);
    }
}

void App::drawPopup() {
    const ThemeColors& tc = themeColors();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);  // the os window has the corners
    ImGui::PushStyleColor(ImGuiCol_WindowBg, tc.panel);
    ImGui::Begin("Clipper popup", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    const ImGuiStyle& style = ImGui::GetStyle();
    const float em = ImGui::GetFontSize();

    popupKeys();

    // search + view switch + "open the manager"
    const bool pinnedView = popupFilter_.view == ClipFilter::View::Pinned;
    const float moreW = ImGui::GetFrameHeight();
    float segW = style.FramePadding.x * 4 + ImGui::CalcTextSize("History").x + ImGui::CalcTextSize("Pinned").x +
                 std::round(style.FramePadding.x * 0.3f) * 2;
    if (popupFocusSearch_) {
        ImGui::SetKeyboardFocusHere();
        popupFocusSearch_ = false;
    }
    // keep the box focused: clicks on rows paste, arrows move, everything else is typing
    if (!ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) ImGui::SetKeyboardFocusHere();
    if (searchField("##q", pinnedView ? "Search pinned" : "Search clipboard history", &popupFilter_.query,
                    -(segW + moreW + style.ItemSpacing.x * 2))) {
        popupRow_ = 0;
        refilter();
    }
    ImGui::SameLine();
    int view = pinnedView ? 1 : 0;
    if (segmented("view", {"History", "Pinned"}, &view)) {
        popupFilter_.view = view ? ClipFilter::View::Pinned : ClipFilter::View::History;
        popupRow_ = 0;
        refilter();
        popupFocusSearch_ = true;
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
    if (ImGui::Button("...", ImVec2(moreW, 0))) windowRequest_ = WindowRequest::Manager;
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open Clipper (Ctrl+M)");

    // list, then a preview of the selected clip, then the key hints
    const float hintsH = ImGui::GetTextLineHeightWithSpacing();
    const float previewH = std::floor(ImGui::GetContentRegionAvail().y * 0.28f);
    const float listH = ImGui::GetContentRegionAvail().y - previewH - hintsH - style.ItemSpacing.y * 2;
    const float rowH = std::round(ImGui::GetTextLineHeight() * 2.1f);
    const float iconS = std::round(ImGui::GetTextLineHeight() * 1.15f);
    const float rowGap = std::round(em * 0.15f);

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, rowGap));
    ImGui::BeginChild("list", ImVec2(0, listH), ImGuiChildFlags_None);
    if (popupResults_.empty()) {
        ImGui::Spacing();
        hint(popupFilter_.query.empty()
                 ? (pinnedView ? "Nothing pinned yet. Ctrl+P pins the selected clip."
                               : "Nothing here yet. Copy something (Ctrl+C) and it shows up here.")
                 : "No clip matches.");
    }
    static int lastRow = -1;
    const bool rowMoved = lastRow != popupRow_;
    lastRow = popupRow_;
    const bool mouseMoved = ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0;

    pushRowColors();
    ImGuiListClipper clip;
    clip.Begin(static_cast<int>(popupResults_.size()), rowH + rowGap);
    if (rowMoved) clip.IncludeItemByIndex(popupRow_);
    while (clip.Step()) {
        for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row) {
            const Clip* c = clipAt(popupResults_, row);
            if (!c) continue;
            ImGui::PushID(static_cast<int>(c->id));
            const bool selected = row == popupRow_;
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##row", selected, ImGuiSelectableFlags_None, ImVec2(0, rowH))) pick(c->id, true);
            const bool hovered = ImGui::IsItemHovered();
            if (hovered && mouseMoved) popupRow_ = row;  // like a menu, but keyboard wins until the mouse moves
            rowBackground(selected, false);
            if (selected && rowMoved) ImGui::SetScrollHereY(0.5f);
            const float x1 = p.x + ImGui::GetItemRectSize().x;
            const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
            const ImU32 dim = ImGui::GetColorU32(tc.dimText);
            const float ty = std::round(p.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f);
            ImDrawList* dl = ImGui::GetWindowDrawList();

            // number for Ctrl+1..9
            float x = p.x + std::round(em * 0.6f);
            if (row < 9) {
                char num[12];
                std::snprintf(num, sizeof num, "%d", row + 1);
                dl->AddText(ImVec2(x, ty), selected ? dim : tc.faint, num);
            }
            x += em * 1.1f;
            drawKindIcon(*c, ImVec2(x, p.y + (rowH - iconS) * 0.5f), iconS, selected);
            x += iconS + em * 0.7f;

            const std::string age = formatAge(c->lastUsed);
            const float ageW = ImGui::CalcTextSize(age.c_str()).x;
            float right = x1 - em * 0.7f - ageW;
            dl->AddText(ImVec2(right, ty), dim, age.c_str());
            if (c->pinned) {
                right -= em * 1.1f;
                iconPin(ImVec2(right + em * 0.45f, ty + ImGui::GetTextLineHeight() * 0.5f), em * 0.42f, tc.accent, true);
            }
            const std::string line = c->title.empty() ? previewLine(*c, 200) : c->title + "  —  " + previewLine(*c, 160);
            clippedText(ImVec2(x, ty), right - em * 0.6f, line, text);
            ImGui::PopID();
        }
    }
    popRowColors();
    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGui::Separator();
    if (const Clip* c = clipAt(popupResults_, popupRow_)) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(tc.dimText));
        drawPreview(*c, previewH - style.ItemSpacing.y);
        ImGui::PopStyleColor();
    } else {
        ImGui::Dummy(ImVec2(0, previewH - style.ItemSpacing.y));
    }
    ImGui::PushStyleColor(ImGuiCol_Text, tc.faint);
    ImGui::TextUnformatted(settings_.autoPaste ? "Enter paste   ·   Ctrl+Enter copy   ·   Ctrl+P pin   ·   Shift+Del delete   ·   Tab pinned"
                                               : "Enter copy   ·   Ctrl+P pin   ·   Shift+Del delete   ·   Tab pinned");
    ImGui::PopStyleColor();
    ImGui::End();
}

// ---- manager ----

void App::selectClip(uint64_t id) {
    selectedId_ = id;
    scrollToSelected_ = true;
}

void App::managerKeys() {
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) return;
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) focusManagerSearch_ = true;
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Equal)) setZoom(settings_.fontZoom + 0.1f);
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Minus)) setZoom(settings_.fontZoom - 0.1f);
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_0, false)) setZoom(1.0f);

    const int n = static_cast<int>(managerResults_.size());
    auto it = std::find(managerResults_.begin(), managerResults_.end(), selectedId_);
    const int row = it == managerResults_.end() ? -1 : static_cast<int>(it - managerResults_.begin());
    auto go = [&](int r) {
        if (n == 0) return;
        r = std::clamp(r, 0, n - 1);
        selectClip(managerResults_[static_cast<size_t>(r)]);
    };
    // arrows always drive the list, the search box is single line anyway
    const bool inText = ImGui::IsAnyItemActive() && ImGui::GetActiveID() != managerSearchId_;
    if (!inText) {
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) go(row + 1);
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) go(row - 1);
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) go(row + 10);
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) go(row - 10);
    }
    if (!selectedId_ || inText) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
        copyToClipboard(selectedId_);
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_P, false))
        if (const Clip* c = store_.find(selectedId_)) store_.setPinned(c->id, !c->pinned);
    if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) focusTitle_ = true;
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && (!ImGui::IsAnyItemActive() || io.KeyShift)) deleteClip(selectedId_);
}

void App::drawManager() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("Clipper", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoSavedSettings);
    drawMenuBar();
    managerKeys();

    const float em = ImGui::GetFontSize();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float statusH = ImGui::GetTextLineHeightWithSpacing() + style.ItemSpacing.y;
    const float bodyH = ImGui::GetContentRegionAvail().y - statusH;
    const float sidebarW = std::round(em * 12.5f);
    const float gap = std::round(em * 0.75f);
    const float detailsW = std::round(std::max(em * 18, ImGui::GetContentRegionAvail().x * 0.38f));

    // sidebar straight on the window, list + details as cards
    ImGui::BeginChild("sidebar", ImVec2(sidebarW, bodyH), ImGuiChildFlags_None);
    drawSidebar();
    ImGui::EndChild();
    ImGui::SameLine(0, gap);
    beginCard("middle", ImVec2(ImGui::GetContentRegionAvail().x - detailsW - gap, bodyH));
    drawClipList();
    ImGui::EndChild();
    ImGui::SameLine(0, gap);
    beginCard("details", ImVec2(0, bodyH));
    drawDetails();
    ImGui::EndChild();

    drawStatusBar();
    drawModals();

    if (preferences_.draw() == PreferencesDialog::Result::Ok) applyPreferences(preferences_.edited());
    ImGui::End();
}

void App::drawMenuBar() {
    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::BeginMenu("Clipper")) {
        if (ImGui::MenuItem("Show popup", prettyAccel(settings_.shortcut).c_str())) openPopup();
        bool paused = !settings_.capturing;
        if (ImGui::MenuItem("Pause capturing", nullptr, &paused)) setCapturing(!paused);
        ImGui::Separator();
        if (ImGui::MenuItem("Preferences...")) preferences_.open(settings_);
        ImGui::Separator();
        if (ImGui::MenuItem("Clear history...")) openClearConfirm_ = true;
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Ctrl+Q")) quit_ = true;
        ImGui::EndMenu();
    }
    const Clip* sel = store_.find(selectedId_);
    if (ImGui::BeginMenu("Clip")) {
        if (ImGui::MenuItem("Copy to clipboard", "Enter", false, sel)) copyToClipboard(selectedId_);
        if (ImGui::MenuItem(sel && sel->pinned ? "Unpin" : "Pin", "Ctrl+P", false, sel))
            store_.setPinned(sel->id, !sel->pinned);
        if (ImGui::MenuItem("Rename", "F2", false, sel)) focusTitle_ = true;
        ImGui::Separator();
        if (ImGui::MenuItem("Delete", "Del", false, sel)) deleteClip(selectedId_);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Zoom In", "Ctrl+=")) setZoom(settings_.fontZoom + 0.1f);
        if (ImGui::MenuItem("Zoom Out", "Ctrl+-")) setZoom(settings_.fontZoom - 0.1f);
        if (ImGui::MenuItem("Actual Size", "Ctrl+0")) setZoom(1.0f);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("Keyboard Shortcuts")) openShortcuts_ = true;
        if (ImGui::MenuItem("About Clipper")) openAbout_ = true;
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Q, false)) quit_ = true;
}

void App::dropTarget(int view, uint32_t folder) {
    if (!ImGui::BeginDragDropTarget()) return;
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("CLIPPER_CLIP")) {
        const uint64_t id = *static_cast<const uint64_t*>(p->Data);
        if (view == kViewPinned) store_.setPinned(id, true);
        if (view == kViewFolder) store_.setFolder(id, folder);
        if (view == kViewHistory) store_.setFolder(id, 0);  // out of its folder
    }
    ImGui::EndDragDropTarget();
}

void App::drawSidebar() {
    const ThemeColors& tc = themeColors();
    const float em = ImGui::GetFontSize();
    const float lineH = ImGui::GetTextLineHeight();
    const float rowH = std::round(lineH * 2.0f);
    enum class Icon { Clock, Pin, Folder };
    pushRowColors();

    auto entry = [&](const char* label, size_t count, ClipFilter::View view, uint32_t folder, Icon icon) {
        const bool on = managerFilter_.view == view && (view != ClipFilter::View::Folder || managerFilter_.folder == folder);
        char id[48];
        std::snprintf(id, sizeof id, "##v%d_%u", static_cast<int>(view), folder);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable(id, on, ImGuiSelectableFlags_None, ImVec2(0, rowH))) {
            managerFilter_.view = view;
            managerFilter_.folder = folder;
            refilter();
        }
        rowBackground(on, ImGui::IsItemHovered());
        const float w = ImGui::GetItemRectSize().x;
        const float cy = p.y + rowH * 0.5f;
        const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
        const ImU32 iconColor = on ? text : tc.fileIcon;
        const ImVec2 ic(p.x + em * 1.1f, cy);
        if (icon == Icon::Clock) iconClock(ic, em * 0.45f, iconColor);
        if (icon == Icon::Pin) iconPin(ic, em * 0.45f, iconColor);
        if (icon == Icon::Folder) iconFolder(ic, em * 0.48f, iconColor);
        const float pillW = countPill(p.x + w - em * 0.55f, cy, count);
        clippedText(ImVec2(p.x + em * 2.2f, std::round(cy - lineH * 0.5f)), p.x + w - pillW - em * 0.9f, label, text);
    };

    ImGui::Dummy(ImVec2(0, em * 0.1f));
    entry("History", countAll_, ClipFilter::View::History, 0, Icon::Clock);
    dropTarget(kViewHistory, 0);
    entry("Pinned", countPinned_, ClipFilter::View::Pinned, 0, Icon::Pin);
    dropTarget(kViewPinned, 0);

    // "Folders" with a round + at the end of the line
    ImGui::Dummy(ImVec2(0, em * 0.6f));
    const ImVec2 hp = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(hp.x + em * 0.5f, hp.y));
    sectionLabel("Folders");
    const float plusR = std::round(lineH * 0.55f);
    const ImVec2 plusC(hp.x + ImGui::GetContentRegionAvail().x - em * 0.55f - plusR, hp.y + lineH * 0.5f);
    ImGui::SetCursorScreenPos(ImVec2(plusC.x - plusR, plusC.y - plusR));
    if (ImGui::InvisibleButton("new_folder", ImVec2(plusR * 2, plusR * 2))) {
        renamingFolder_ = 0;
        moveAfterCreate_ = 0;
        folderNameEdit_.clear();
        openFolderName_ = true;
    }
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const bool hot = ImGui::IsItemHovered();
        const ImU32 col = hot ? ImGui::GetColorU32(ImGuiCol_Text) : ImGui::GetColorU32(tc.dimText);
        if (hot) dl->AddCircleFilled(plusC, plusR, tc.hover, 20);
        dl->AddCircle(plusC, plusR, tc.hairline, 20, 1.0f);
        const float s = plusR * 0.45f, t = strokeWidth();
        dl->AddLine(ImVec2(plusC.x - s, plusC.y), ImVec2(plusC.x + s, plusC.y), col, t);
        dl->AddLine(ImVec2(plusC.x, plusC.y - s), ImVec2(plusC.x, plusC.y + s), col, t);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("New folder");
    ImGui::SetCursorScreenPos(ImVec2(hp.x, hp.y + lineH + ImGui::GetStyle().ItemSpacing.y));

    const std::vector<Folder>& folders = store_.folders();
    if (folders.empty()) {
        ImGui::Indent(em * 0.5f);
        hint("Drag clips here to keep them. Folders never get cleaned up.");
        ImGui::Unindent(em * 0.5f);
    }
    uint32_t removeFolder = 0;
    for (size_t i = 0; i < folders.size(); ++i) {
        const Folder& f = folders[i];
        entry(f.name.c_str(), i < folderCounts_.size() ? folderCounts_[i] : 0, ClipFilter::View::Folder, f.id, Icon::Folder);
        dropTarget(kViewFolder, f.id);
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Rename...")) {
                renamingFolder_ = f.id;
                folderNameEdit_ = f.name;
                openFolderName_ = true;
            }
            if (ImGui::MenuItem("Delete folder")) removeFolder = f.id;
            ImGui::TextColored(tc.dimText, "Its clips go back to the history");
            ImGui::EndPopup();
        }
    }
    if (removeFolder) store_.removeFolder(removeFolder);

    // type filter at the bottom, same rows without counts
    const float typesH = rowH * 4 + ImGui::GetStyle().ItemSpacing.y * 4 + lineH + em * 0.5f;
    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY() + em, ImGui::GetWindowHeight() - typesH));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + em * 0.5f);
    sectionLabel("Show");
    const struct {
        const char* label;
        ClipFilter::Type type;
    } types[] = {{"Everything", ClipFilter::Type::Any},
                 {"Text", ClipFilter::Type::Text},
                 {"Images", ClipFilter::Type::Image},
                 {"Files", ClipFilter::Type::Files}};
    const float typeH = std::round(lineH * 1.75f);
    for (const auto& t : types) {
        const bool on = managerFilter_.type == t.type;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushID(t.label);
        if (ImGui::Selectable("##type", on, ImGuiSelectableFlags_None, ImVec2(0, typeH))) {
            managerFilter_.type = t.type;
            refilter();
        }
        ImGui::PopID();
        rowBackground(on, ImGui::IsItemHovered());
        const float cy = p.y + typeH * 0.5f;
        // a dot like a radio, filled with the accent when on
        const ImVec2 dot(p.x + em * 1.1f, cy);
        if (on)
            ImGui::GetWindowDrawList()->AddCircleFilled(dot, em * 0.2f, tc.accent, 12);
        else
            ImGui::GetWindowDrawList()->AddCircle(dot, em * 0.2f, tc.fileIcon, 12, 1.0f);
        ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + em * 2.2f, std::round(cy - lineH * 0.5f)),
                                            on ? ImGui::GetColorU32(ImGuiCol_Text) : ImGui::GetColorU32(tc.dimText), t.label);
    }
    popRowColors();
}

void App::drawClipContextMenu(const Clip& clip) {
    if (!ImGui::BeginPopupContextItem("clip_menu")) return;
    selectedId_ = clip.id;
    if (ImGui::MenuItem("Copy to clipboard", "Enter")) copyToClipboard(clip.id);
    if (ImGui::MenuItem(clip.pinned ? "Unpin" : "Pin", "Ctrl+P")) store_.setPinned(clip.id, !clip.pinned);
    if (ImGui::BeginMenu("Move to folder")) {
        if (ImGui::MenuItem("None", nullptr, clip.folder == 0)) store_.setFolder(clip.id, 0);
        for (const Folder& f : store_.folders()) {
            if (ImGui::MenuItem(f.name.c_str(), nullptr, clip.folder == f.id)) store_.setFolder(clip.id, f.id);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("New folder...")) {
            renamingFolder_ = 0;
            moveAfterCreate_ = clip.id;
            folderNameEdit_.clear();
            openFolderName_ = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Rename", "F2")) focusTitle_ = true;
    if (clip.kind == ClipKind::Files) {
        const std::vector<std::string> paths = clipPaths(clip);
        if (!paths.empty() && ImGui::MenuItem("Open containing folder")) {
            if (!showInFileManager(paths[0])) setStatus("Could not open the folder of " + paths[0]);
        }
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Delete", "Del")) deleteClip(clip.id);
    ImGui::EndPopup();
}

void App::drawClipList() {
    const ThemeColors& tc = themeColors();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float em = ImGui::GetFontSize();

    if (focusManagerSearch_) {
        ImGui::SetKeyboardFocusHere();
        focusManagerSearch_ = false;
    }
    if (searchField("##search", "Search  (Ctrl+F)", &managerFilter_.query, -1)) refilter();
    managerSearchId_ = ImGui::GetItemID();
    ImGui::Dummy(ImVec2(0, em * 0.1f));

    const float lineH = ImGui::GetTextLineHeight();
    const float rowH = std::round(lineH * 2.9f);
    const float thumbS = std::round(rowH - style.FramePadding.y * 2.2f);
    const float rowGap = std::round(em * 0.2f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, rowGap));
    ImGui::BeginChild("rows", ImVec2(0, 0), ImGuiChildFlags_None);
    if (managerResults_.empty()) {
        ImGui::Spacing();
        hint(!managerFilter_.query.empty()                          ? "No clip matches."
             : managerFilter_.view == ClipFilter::View::History    ? "Nothing copied yet. Everything you copy (Ctrl+C) lands here."
             : managerFilter_.view == ClipFilter::View::Pinned     ? "Nothing pinned yet. Ctrl+P pins the selected clip."
                                                                    : "Empty folder. Drag clips onto it in the sidebar.");
    }

    pushRowColors();
    ImGuiListClipper clip;
    clip.Begin(static_cast<int>(managerResults_.size()), rowH + rowGap);
    int selectedRow = -1;
    if (scrollToSelected_) {
        auto it = std::find(managerResults_.begin(), managerResults_.end(), selectedId_);
        if (it != managerResults_.end()) {
            selectedRow = static_cast<int>(it - managerResults_.begin());
            clip.IncludeItemByIndex(selectedRow);
        }
    }
    while (clip.Step()) {
        for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row) {
            const Clip* c = clipAt(managerResults_, row);
            if (!c) continue;
            ImGui::PushID(static_cast<int>(c->id));
            const bool selected = c->id == selectedId_;
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##row", selected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, rowH))) {
                selectedId_ = c->id;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) copyToClipboard(c->id);
            }
            rowBackground(selected, ImGui::IsItemHovered());
            if (row == selectedRow && scrollToSelected_) {
                ImGui::SetScrollHereY(0.5f);
                scrollToSelected_ = false;
            }
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("CLIPPER_CLIP", &c->id, sizeof c->id);
                ImGui::TextUnformatted(previewLine(*c, 40).c_str());
                ImGui::EndDragDropSource();
            }
            drawClipContextMenu(*c);

            const float x1 = p.x + ImGui::GetItemRectSize().x;
            const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
            const ImU32 dim = ImGui::GetColorU32(tc.dimText);
            ImDrawList* dl = ImGui::GetWindowDrawList();

            // icon / thumbnail on a small tile
            float x = p.x + std::round(em * 0.55f);
            const ImVec2 tile(x, p.y + (rowH - thumbS) * 0.5f);
            if (c->kind != ClipKind::Image)
                dl->AddRectFilled(tile, ImVec2(tile.x + thumbS, tile.y + thumbS), tc.pill, style.FrameRounding);
            const float s = c->kind == ClipKind::Image ? thumbS : std::round(lineH * 1.15f);
            drawKindIcon(*c, ImVec2(tile.x + (thumbS - s) * 0.5f, tile.y + (thumbS - s) * 0.5f), s, selected);
            x += thumbS + em * 0.75f;

            // title line with the age on the right, details under it
            const float y1 = std::round(p.y + (rowH - lineH * 2 - em * 0.25f) * 0.5f);
            const float y2 = std::round(y1 + lineH + em * 0.25f);
            const std::string age = formatAge(c->lastUsed);
            const float ageW = ImGui::CalcTextSize(age.c_str()).x;
            float right = x1 - em * 0.75f - ageW;
            dl->AddText(ImVec2(right, y1), dim, age.c_str());
            if (c->pinned) {
                right -= em * 1.1f;
                iconPin(ImVec2(right + em * 0.45f, y1 + lineH * 0.5f), em * 0.42f, tc.accent, true);
            }
            clippedText(ImVec2(x, y1), right - em * 0.6f, c->title.empty() ? previewLine(*c, 300) : c->title, text);

            std::string meta = c->source;
            auto add = [&](const std::string& part) { meta += (meta.empty() ? "" : "  ·  ") + part; };
            if (const Folder* f = c->folder ? store_.findFolder(c->folder) : nullptr) add(f->name);
            if (c->kind == ClipKind::Text) add(formatSize(c->bytes));
            if (c->kind == ClipKind::Image) add(std::to_string(c->width) + " × " + std::to_string(c->height));
            if (c->kind == ClipKind::Files && c->cut) add("cut");
            if (!c->title.empty() && c->kind != ClipKind::Image) add(previewLine(*c, 120));  // images: size is there already
            clippedText(ImVec2(x, y2), x1 - em * 0.75f, meta, dim);
            ImGui::PopID();
        }
    }
    popRowColors();
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void App::drawDetails() {
    const ThemeColors& tc = themeColors();
    const Clip* c = store_.find(selectedId_);
    if (!c) {
        ImGui::Spacing();
        hint("Pick a clip on the left to see all of it, give it a title, edit it or file it away.");
        ImGui::Spacing();
        hint("Enter puts it back on the clipboard. Double click does too.");
        editingId_ = 0;
        return;
    }
    if (editingId_ != c->id) {
        editingId_ = c->id;
        titleEdit_ = c->title;
        textEdit_ = c->kind == ClipKind::Text && c->text.size() <= kMaxEditableText ? c->text : std::string();
    }

    const float em = ImGui::GetFontSize();

    // title, pin, folder, actions
    if (focusTitle_) {
        ImGui::SetKeyboardFocusHere();
        focusTitle_ = false;
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##title", "Add a title  (F2)", &titleEdit_);
    if (ImGui::IsItemDeactivatedAfterEdit()) store_.setTitle(c->id, titleEdit_);

    bool pinned = c->pinned;
    if (toggleSwitch("Pinned", &pinned)) store_.setPinned(c->id, pinned);
    ImGui::SameLine();
    const Folder* folder = c->folder ? store_.findFolder(c->folder) : nullptr;
    ImGui::SetNextItemWidth(em * 9);
    if (ImGui::BeginCombo("##folder", folder ? folder->name.c_str() : "No folder")) {
        if (ImGui::Selectable("No folder", !folder)) store_.setFolder(c->id, 0);
        for (const Folder& f : store_.folders()) {
            if (ImGui::Selectable(f.name.c_str(), c->folder == f.id)) store_.setFolder(c->id, f.id);
        }
        ImGui::EndCombo();
    }
    // actions on the right end of the line
    const ImGuiStyle& style = ImGui::GetStyle();
    const float actionsW = ImGui::CalcTextSize("Copy").x + ImGui::CalcTextSize("Delete").x + style.FramePadding.x * 4 +
                           style.ItemSpacing.x;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - actionsW));
    if (ImGui::Button("Copy")) copyToClipboard(c->id);
    ImGui::SameLine();
    if (ImGui::Button("Delete")) {
        deleteClip(c->id);
        return;
    }

    std::string info = std::string(kindName(c->kind));
    if (!c->source.empty()) info += " from " + c->source;
    info += "  ·  first " + formatTime(c->created) + "  ·  used " + std::to_string(c->useCount) + "×";
    ImGui::TextColored(tc.dimText, "%s", info.c_str());
    ImGui::Dummy(ImVec2(0, em * 0.1f));

    const float footer = ImGui::GetFrameHeightWithSpacing();
    if (c->kind == ClipKind::Text && c->text.size() <= kMaxEditableText) {
        const bool edited = textEdit_ != c->text;
        ImGui::InputTextMultiline("##text", &textEdit_, ImVec2(-1, ImGui::GetContentRegionAvail().y - footer));
        ImGui::BeginDisabled(!edited || textEdit_.empty());
        if (ImGui::Button("Save changes")) {
            if (!store_.setText(c->id, textEdit_)) setStatus("That exact text is already another clip");
        }
        ImGui::SameLine();
        if (ImGui::Button("Revert")) textEdit_ = c->text;
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextColored(tc.dimText, "%s", formatSize(c->bytes).c_str());
    } else {
        drawPreview(*c, ImGui::GetContentRegionAvail().y - footer);
        if (c->kind == ClipKind::Files) {
            const std::vector<std::string> paths = clipPaths(*c);
            if (!paths.empty() && ImGui::Button("Open containing folder")) {
                if (!showInFileManager(paths[0])) setStatus("Could not open the folder of " + paths[0]);
            }
            ImGui::SameLine();
            ImGui::TextColored(tc.dimText, "Paste into a folder in your file manager to copy them there");
        } else {
            ImGui::TextColored(tc.dimText, "%s", formatSize(c->bytes).c_str());
        }
    }
}

void App::drawStatusBar() {
    const ThemeColors& tc = themeColors();
    std::string left;
    const bool fresh = !statusMessage_.empty() && std::chrono::steady_clock::now() - statusTime_ < std::chrono::seconds(6);
    if (fresh)
        left = statusMessage_;
    else if (!storeError_.empty())
        left = "History is not being saved: " + storeError_;
    else if (!clipboard_->available())
#ifdef _WIN32
        left = "Can't watch the clipboard, nothing gets captured";
#else
        left = "No X11 clipboard, nothing gets captured (Wayland isn't supported yet)";
#endif
    else if (!settings_.capturing)
        left = "Paused, copies aren't saved. Clipper > Pause capturing to resume";
    else
        left = std::to_string(countAll_) + " clips  ·  " + std::to_string(store_.keptCount()) + " kept  ·  " +
               formatSize(store_.diskBytes()) + " on disk";
    if (fresh) {  // the status ages out on its own, make sure a frame gets drawn when it does
        if (wake_) wake_();
    }
    ImGui::TextColored(!settings_.capturing && !fresh ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : tc.dimText, "%s",
                       left.c_str());
}

void App::drawModals() {
    const float em = ImGui::GetFontSize();
    if (openClearConfirm_) {
        ImGui::OpenPopup("Clear history?");
        openClearConfirm_ = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Clear history?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const size_t loose = store_.clips().size() - store_.keptCount();
        ImGui::Text("Delete %zu clips from the history?", loose);
        hint("Pinned clips and clips in folders stay.");
        ImGui::Spacing();
        if (ImGui::Button("Delete", ImVec2(em * 6, 0))) {
            const size_t n = store_.clearHistory();
            setStatus("Deleted " + std::to_string(n) + " clips");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(em * 6, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (openFolderName_) {
        ImGui::OpenPopup("Folder");
        openFolderName_ = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Folder", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(renamingFolder_ ? "Rename folder" : "New folder");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(em * 16);
        const bool enter = ImGui::InputText("##name", &folderNameEdit_, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::BeginDisabled(folderNameEdit_.empty());
        if (ImGui::Button("OK", ImVec2(em * 6, 0)) || (enter && !folderNameEdit_.empty())) {
            if (renamingFolder_) {
                store_.renameFolder(renamingFolder_, folderNameEdit_);
            } else {
                const uint32_t id = store_.addFolder(folderNameEdit_);
                if (moveAfterCreate_) store_.setFolder(moveAfterCreate_, id);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(em * 6, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (openAbout_) {
        ImGui::OpenPopup("About Clipper");
        openAbout_ = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("About Clipper", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Clipper %s", CLIPPER_VERSION);
#ifdef _WIN32
        hint("Clipboard history for Windows and Linux, inspired by ClipClip.");
#else
        hint("Clipboard history for Linux and Windows, inspired by ClipClip.");
#endif
        ImGui::TextColored(themeColors().dimText, "History: %s", dataDirectory().c_str());
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(em * 6, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (openShortcuts_) {
        ImGui::OpenPopup("Keyboard Shortcuts");
        openShortcuts_ = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Keyboard Shortcuts", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const std::string popupKey = prettyAccel(settings_.shortcut);
        const char* rows[][2] = {
            {popupKey.c_str(), "open the popup from any app"},
            {"", ""},
            {"Popup", ""},
            {"type", "search"},
            {"Up / Down", "pick a clip"},
            {"Enter / click", settings_.autoPaste ? "paste it" : "copy it"},
            {"Ctrl+Enter", "copy without pasting"},
            {"Ctrl+1 ... 9", "paste that row"},
            {"Tab", "history / pinned"},
            {"Ctrl+P", "pin / unpin"},
            {"Shift+Del", "delete"},
            {"Ctrl+M", "open the manager"},
            {"Esc", "close"},
            {"", ""},
            {"Manager", ""},
            {"Enter / double click", "copy to clipboard"},
            {"Ctrl+F", "search"},
            {"F2", "title"},
            {"Del", "delete"},
            {"drag", "into Pinned or a folder"},
        };
        if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_SizingFixedFit)) {
            for (const auto& r : rows) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (r[1][0] == '\0' && r[0][0])
                    ImGui::TextColored(themeColors().dimText, "%s", r[0]);
                else
                    ImGui::TextUnformatted(r[0]);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(r[1]);
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(em * 6, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ---- screenshots ----

void App::showcase(bool popup) {
    if (store_.clips().empty()) {
        // what a few days of copying looks like. oldest first, every clip a few minutes after the one before
        const int64_t now = nowMs();
        int64_t t = now - (3070 + 2) * 60 * 1000LL;  // the minutes below add up to 3070: the newest is from just now
        auto add = [&](ClipKind kind, std::string text, const char* source, int64_t minutesLater, std::string png = {}) {
            t = std::min(now - 60 * 1000, t + minutesLater * 60 * 1000);
            NewClip c;
            c.kind = kind;
            c.text = std::move(text);
            c.png = std::move(png);
            c.source = source;
            return store_.add(std::move(c), t);
        };
#ifdef _WIN32
        const char *terminal = "WindowsTerminal", *browser = "chrome", *editor = "Code", *chat = "Slack",
                   *files = "explorer", *paint = "mspaint", *mail = "OUTLOOK";
        const std::string report = "file:///C:/Users/Public/Documents/Report%202026.pdf\nfile:///C:/Users/Public/Documents/Budget.xlsx";
#else
        const char *terminal = "Gnome-terminal", *browser = "firefox", *editor = "Code", *chat = "Slack", *files = "Nemo",
                   *paint = "Gimp-2.10", *mail = "thunderbird";
        const std::string report = "file:///home/sam/Documents/Report%202026.pdf\nfile:///home/sam/Documents/Budget.ods";
#endif
        const uint32_t snippets = store_.addFolder("Snippets");
        const uint32_t work = store_.addFolder("Work");
        const uint64_t reply = add(ClipKind::Text,
                                   "Thanks for reaching out! I'm away until Monday and will get back to you then.", mail, 0);
        store_.setTitle(reply, "Out of office");
        store_.setFolder(reply, snippets);
        add(ClipKind::Text, "#1F9EDE", editor, 400);
        const uint64_t addr = add(ClipKind::Text, "221B Baker Street\nLondon NW1 6XE\nUnited Kingdom", mail, 300);
        store_.setTitle(addr, "Shipping address");
        store_.setFolder(addr, snippets);
        add(ClipKind::Files, report, files, 700);
        const uint64_t notes = add(ClipKind::Text,
                                   "Standup moved to 10:30. Demo on Thursday, bring the release notes and the new screenshots.",
                                   chat, 200);
        store_.setFolder(notes, work);
        add(ClipKind::Image, {}, paint, 900, std::string(reinterpret_cast<const char*>(kAppIconPng), sizeof kAppIconPng));
        const uint64_t link = add(ClipKind::Text, "https://github.com/KimsFerdy/Clipper/releases/latest", browser, 400);
        store_.setPinned(link, true);
        add(ClipKind::Text, "for (const Clip& c : store.clips())\n    total += c.bytes;", editor, 120);
        add(ClipKind::Text, "git log --oneline --graph --decorate -20", terminal, 30);
        add(ClipKind::Text, "Clipboard history that stays out of your way.", browser, 20);
    }
    refilter();
    if (popup) {
        popupFilter_ = {};
        popupRow_ = 1;
        refilter();
        windowChanged(Mode::Popup);
        popupFocusSearch_ = false;  // no blinking cursor in the picture
    } else {
        windowChanged(Mode::Manager);
        focusManagerSearch_ = false;
        if (managerResults_.size() > 1) selectedId_ = managerResults_[1];
    }
}

}  // namespace clipper
