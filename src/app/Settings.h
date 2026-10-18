#pragma once

#include <string>
#include <vector>

namespace clipper {

struct WindowGeometry {
    int x = -1, y = -1;  // -1 = let the window manager pick
    int width = 1000, height = 640;
    bool maximized = false;

    bool operator==(const WindowGeometry&) const = default;
};

// ~/.config/clipper/settings.ini, plain key = value so you can edit it by hand
struct Settings {
    enum class Theme { System, Dark, Light };

    bool firstRun = true;        // no settings file yet. not saved
    bool launchAtLogin = false;  // not in the ini, mirrors the autostart file
    bool trayIcon = true;
    bool closeToTray = true;  // closing the window keeps clipper running (and capturing) in the tray

    bool popupShortcut = true;                // set up on the first run, it's the whole point
    std::string shortcut = "<Primary><Shift>v";  // gtk accelerator
    bool autoPaste = true;                    // picking a clip in the popup pastes it right away

    bool capturing = true;  // tray > Pause capturing
    bool captureImages = true;
    int maxClips = 1000;  // history size, pinned / foldered clips don't count
    std::vector<std::string> excludedApps = {"KeePassXC", "Bitwarden", "1Password"};  // WM_CLASS, any case

    Theme theme = Theme::System;
    bool customAccent = false;
    float accent[3] = {0.12f, 0.62f, 0.87f};
    float fontZoom = 1.0f;

    WindowGeometry window;
    int popupWidth = 520, popupHeight = 460;

    bool operator==(const Settings&) const = default;
};

Settings loadSettings();
bool saveSettings(const Settings& settings);

}  // namespace clipper
