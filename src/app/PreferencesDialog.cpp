#include "app/PreferencesDialog.h"

#include <algorithm>
#include <cmath>

#include "app/Theme.h"
#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"
#include "platform/GlobalShortcut.h"

namespace clipper {

namespace {

constexpr const char* kTitle = "Preferences";

// imgui key -> gtk accelerator key name
struct KeyName {
    ImGuiKey key;
    const char* name;
};
constexpr KeyName kKeyNames[] = {
    {ImGuiKey_Space, "space"},
    {ImGuiKey_Comma, "comma"},
    {ImGuiKey_Period, "period"},
    {ImGuiKey_Slash, "slash"},
    {ImGuiKey_Semicolon, "semicolon"},
    {ImGuiKey_Apostrophe, "apostrophe"},
    {ImGuiKey_Minus, "minus"},
    {ImGuiKey_Equal, "equal"},
    {ImGuiKey_GraveAccent, "grave"},
    {ImGuiKey_Backslash, "backslash"},
    {ImGuiKey_LeftBracket, "bracketleft"},
    {ImGuiKey_RightBracket, "bracketright"},
    {ImGuiKey_Insert, "Insert"},
    {ImGuiKey_Home, "Home"},
    {ImGuiKey_End, "End"},
    {ImGuiKey_PageUp, "Page_Up"},
    {ImGuiKey_PageDown, "Page_Down"},
    {ImGuiKey_Pause, "Pause"},
};

// the key pressed this frame as a gtk accelerator ("<Super>f"), "" if none / no modifier
std::string pressedAccel() {
    const ImGuiIO& io = ImGui::GetIO();
    std::string key;
    for (int k = ImGuiKey_A; k <= ImGuiKey_Z && key.empty(); ++k) {
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(k), false)) key = std::string(1, char('a' + (k - ImGuiKey_A)));
    }
    for (int k = ImGuiKey_0; k <= ImGuiKey_9 && key.empty(); ++k) {
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(k), false)) key = std::string(1, char('0' + (k - ImGuiKey_0)));
    }
    bool functionKey = false;
    for (int k = ImGuiKey_F1; k <= ImGuiKey_F12 && key.empty(); ++k) {
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(k), false)) {
            key = "F" + std::to_string(k - ImGuiKey_F1 + 1);
            functionKey = true;
        }
    }
    for (const KeyName& kn : kKeyNames) {
        if (key.empty() && ImGui::IsKeyPressed(kn.key, false)) key = kn.name;
    }
    const bool anyMod = io.KeyCtrl || io.KeyAlt || io.KeySuper || io.KeyShift;
    if (key.empty() || (!anyMod && !functionKey)) return {};  // plain letters would eat normal typing
    std::string accel;
    if (io.KeySuper) accel += "<Super>";
    if (io.KeyCtrl) accel += "<Primary>";
    if (io.KeyAlt) accel += "<Alt>";
    if (io.KeyShift) accel += "<Shift>";
    return accel + key;
}

void hint(const char* text) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(themeColors().dimText, "%s", text);
    ImGui::PopTextWrapPos();
}

}  // namespace

void PreferencesDialog::open(const Settings& current) {
    draft_ = current;
    recording_ = false;
    appInput_.clear();
    requestOpen_ = true;
}

PreferencesDialog::Result PreferencesDialog::draw() {
    if (requestOpen_) {
        ImGui::OpenPopup(kTitle);
        requestOpen_ = false;
    }

    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(em * 38, em * 28), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_NoSavedSettings)) return Result::None;

    // tabs take everything except the button row
    const float buttonRow = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    ImGui::BeginChild("tabs", ImVec2(0, -buttonRow));
    if (ImGui::BeginTabBar("prefs_tabs")) {
        if (ImGui::BeginTabItem("General")) {
            drawGeneralTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("History")) {
            drawHistoryTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Appearance")) {
            drawAppearanceTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    Result result = Result::None;
    ImGui::Separator();
    const ImVec2 button(em * 6, 0);
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - button.x * 2 - ImGui::GetStyle().ItemSpacing.x -
                         ImGui::GetStyle().WindowPadding.x);
    if (ImGui::Button("Cancel", button) || (!recording_ && ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
        result = Result::Cancel;
    ImGui::SameLine();
    if (ImGui::Button("OK", button)) result = Result::Ok;
    if (result != Result::None) ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
    return result;
}

void PreferencesDialog::drawGeneralTab() {
    ImGui::Spacing();
    ImGui::Checkbox("Start Clipper when I log in", &draft_.launchAtLogin);
    ImGui::Indent();
    hint("It can only remember what you copy while it's running. Starts in the tray.");
    ImGui::Unindent();

    ImGui::Spacing();
    ImGui::Checkbox("Show icon in the system tray", &draft_.trayIcon);
    ImGui::Indent();
    ImGui::BeginDisabled(!draft_.trayIcon);
    ImGui::Checkbox("Closing the window keeps Clipper running in the tray", &draft_.closeToTray);
    ImGui::EndDisabled();
    hint("Left click the tray icon to open Clipper, right click for the menu.");
    ImGui::Unindent();

    ImGui::Spacing();
    drawShortcutRecorder();

    ImGui::Spacing();
    ImGui::Checkbox("Paste right away when I pick a clip in the popup", &draft_.autoPaste);
    ImGui::Indent();
    hint(
        "Clipper puts the clip on the clipboard, goes back to the app you were in and presses Ctrl+V for you "
        "(Shift+Insert in terminals). Off: it only goes on the clipboard and you paste yourself.");
    ImGui::Unindent();
}

void PreferencesDialog::drawShortcutRecorder() {
    const ShortcutDesktop desktop = shortcutDesktop();
    ImGui::BeginDisabled(desktop == ShortcutDesktop::Unsupported);
    ImGui::Checkbox("Popup shortcut", &draft_.popupShortcut);
    ImGui::SameLine();
    ImGui::BeginDisabled(!draft_.popupShortcut);
    const std::string label = recording_ ? std::string("Press a key combo...") : prettyAccel(draft_.shortcut);
    if (ImGui::Button((label + "###shortcut").c_str(), ImVec2(ImGui::GetFontSize() * 10, 0))) recording_ = true;
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    if (recording_) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            recording_ = false;
        } else if (std::string accel = pressedAccel(); !accel.empty()) {
            draft_.shortcut = accel;
            recording_ = false;
        }
    }

    ImGui::Indent();
    if (desktop == ShortcutDesktop::Unsupported) {
        hint(
            "Can't set it up automatically here. Add a shortcut in your desktop's keyboard settings "
            "that runs: Clipper --popup");
    } else {
        const std::string text = std::string("Opens the history popup at the mouse from any app. Added to ") +
                                 shortcutDesktopName(desktop) +
                                 "'s keyboard shortcuts, you can change it there too. Ctrl+V keeps pasting as usual.";
        hint(text.c_str());
    }
    ImGui::Unindent();
}

void PreferencesDialog::drawHistoryTab() {
    ImGui::Spacing();
    ImGui::Checkbox("Save images too (screenshots, copied pictures)", &draft_.captureImages);
    ImGui::Indent();
    hint("Stored as png next to the history, up to 64 MB each.");
    ImGui::Unindent();

    ImGui::Spacing();
    ImGui::TextUnformatted("Keep the last");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    ImGui::InputInt("clips##max", &draft_.maxClips, 100, 1000);
    draft_.maxClips = std::clamp(draft_.maxClips, 10, 100000);
    ImGui::Indent();
    hint("Older ones get dropped. Pinned clips and clips in folders are kept forever and don't count.");
    ImGui::Unindent();

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::TextUnformatted("Never save copies from these apps");
    hint(
        "Window class names, any case (password managers). Copies that mark themselves as secret "
        "(KeePassXC does) are always skipped.");

    auto& apps = draft_.excludedApps;
    for (size_t i = 0; i < apps.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::SmallButton("Remove")) {
            apps.erase(apps.begin() + static_cast<std::ptrdiff_t>(i));
            ImGui::PopID();
            break;
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(apps[i].c_str());
        ImGui::PopID();
    }

    const float addWidth = ImGui::GetFontSize() * 5;
    ImGui::SetNextItemWidth(-addWidth - ImGui::GetStyle().ItemSpacing.x);
    const bool enter =
        ImGui::InputTextWithHint("##app", "App name, e.g. KeePassXC", &appInput_, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Add", ImVec2(addWidth, 0)) || enter) && !appInput_.empty()) {
        if (std::find(apps.begin(), apps.end(), appInput_) == apps.end()) apps.push_back(appInput_);
        appInput_.clear();
    }
}

void PreferencesDialog::drawAppearanceTab() {
    ImGui::Spacing();
    ImGui::TextUnformatted("Theme");
    int theme = static_cast<int>(draft_.theme);
    ImGui::RadioButton("Follow system", &theme, static_cast<int>(Settings::Theme::System));
    ImGui::SameLine();
    ImGui::RadioButton("Dark", &theme, static_cast<int>(Settings::Theme::Dark));
    ImGui::SameLine();
    ImGui::RadioButton("Light", &theme, static_cast<int>(Settings::Theme::Light));
    draft_.theme = static_cast<Settings::Theme>(theme);

    ImGui::Spacing();
    ImGui::Checkbox("Custom accent colour", &draft_.customAccent);
    ImGui::SameLine();
    ImGui::BeginDisabled(!draft_.customAccent);
    ImGui::ColorEdit3("##accent", draft_.accent, ImGuiColorEditFlags_NoInputs);
    ImGui::EndDisabled();
    hint("Off: use the desktop's accent colour.");

    ImGui::Spacing();
    ImGui::TextUnformatted("Font size");
    int percent = static_cast<int>(std::lround(draft_.fontZoom * 100));
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    if (ImGui::SliderInt("##zoom", &percent, 70, 200, "%d%%")) draft_.fontZoom = static_cast<float>(percent) / 100.0f;
    hint("Also Ctrl+= / Ctrl+- / Ctrl+0 in the main window.");
}

}  // namespace clipper
