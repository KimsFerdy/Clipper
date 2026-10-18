#include "app/Settings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "platform/Autostart.h"
#include "platform/Paths.h"

namespace clipper {

namespace {

std::string trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r");
    const size_t e = s.find_last_not_of(" \t\r");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

bool parseBool(const std::string& v) { return v == "true" || v == "1" || v == "yes"; }

const char* boolText(bool b) { return b ? "true" : "false"; }

}  // namespace

Settings loadSettings() {
    Settings s;
    s.launchAtLogin = isLaunchAtLoginEnabled();

    std::ifstream in(settingsFilePath());
    if (!in) return s;
    s.firstRun = false;
    bool sawExcluded = false;  // the list replaces the defaults, an empty one included

    // sections are just for humans, keys are unique anyway
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));

        if (key == "tray_icon")
            s.trayIcon = parseBool(value);
        else if (key == "close_to_tray")
            s.closeToTray = parseBool(value);
        else if (key == "popup_shortcut")
            s.popupShortcut = parseBool(value);
        else if (key == "shortcut")
            s.shortcut = value;
        else if (key == "auto_paste")
            s.autoPaste = parseBool(value);
        else if (key == "capturing")
            s.capturing = parseBool(value);
        else if (key == "capture_images")
            s.captureImages = parseBool(value);
        else if (key == "max_clips")
            s.maxClips = std::clamp(std::atoi(value.c_str()), 10, 100000);
        else if (key == "excluded_apps") {
            if (!sawExcluded) s.excludedApps.clear();
            sawExcluded = true;
        } else if (key == "exclude_app") {
            if (!sawExcluded) s.excludedApps.clear();
            sawExcluded = true;
            s.excludedApps.push_back(value);
        } else if (key == "theme")
            s.theme = value == "dark"    ? Settings::Theme::Dark
                      : value == "light" ? Settings::Theme::Light
                                         : Settings::Theme::System;
        else if (key == "custom_accent")
            s.customAccent = parseBool(value);
        else if (key == "accent")
            std::sscanf(value.c_str(), "%f %f %f", &s.accent[0], &s.accent[1], &s.accent[2]);
        else if (key == "font_zoom")
            s.fontZoom = std::clamp(std::strtof(value.c_str(), nullptr), 0.7f, 2.0f);
        else if (key == "window")
            std::sscanf(value.c_str(), "%d %d %d %d", &s.window.x, &s.window.y, &s.window.width, &s.window.height);
        else if (key == "window_maximized")
            s.window.maximized = parseBool(value);
        else if (key == "popup_size")
            std::sscanf(value.c_str(), "%d %d", &s.popupWidth, &s.popupHeight);
    }
    return s;
}

bool saveSettings(const Settings& s) {
    const std::string path = settingsFilePath();
    if (path.empty()) return false;

    const char* theme = s.theme == Settings::Theme::Dark    ? "dark"
                        : s.theme == Settings::Theme::Light ? "light"
                                                            : "system";
    {
        std::ofstream out(path + ".tmp", std::ios::trunc);
        out << "# Clipper settings. fine to edit while the app is closed\n\n"
            << "[general]\n"
            << "tray_icon = " << boolText(s.trayIcon) << "\n"
            << "close_to_tray = " << boolText(s.closeToTray) << "\n"
            << "popup_shortcut = " << boolText(s.popupShortcut) << "\n"
            << "shortcut = " << s.shortcut << "\n"
            << "auto_paste = " << boolText(s.autoPaste) << "\n\n"
            << "[history]\n"
            << "capturing = " << boolText(s.capturing) << "\n"
            << "capture_images = " << boolText(s.captureImages) << "\n"
            << "max_clips = " << s.maxClips << "\n"
            << "# copies made in these apps (WM_CLASS) are never saved\n"
            << "excluded_apps =\n";
        for (const std::string& a : s.excludedApps) out << "exclude_app = " << a << "\n";
        out << "\n[appearance]\n"
            << "theme = " << theme << "\n"
            << "custom_accent = " << boolText(s.customAccent) << "\n"
            << "accent = " << s.accent[0] << " " << s.accent[1] << " " << s.accent[2] << "\n"
            << "font_zoom = " << s.fontZoom << "\n\n"
            << "[window]\n"
            << "window = " << s.window.x << " " << s.window.y << " " << s.window.width << " " << s.window.height << "\n"
            << "window_maximized = " << boolText(s.window.maximized) << "\n"
            << "popup_size = " << s.popupWidth << " " << s.popupHeight << "\n";
        if (!out.flush()) return false;
    }
    std::error_code ec;
    std::filesystem::rename(path + ".tmp", path, ec);
    return !ec;
}

}  // namespace clipper
