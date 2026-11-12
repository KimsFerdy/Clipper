// the popup shortcut on windows: RegisterHotKey on the AppBus window, so WM_HOTKEY lands in the
// running app directly (no second process like on linux). lasts as long as we run, App sets it on start

#include <cctype>
#include <cstdlib>

#include "platform/GlobalShortcut.h"
#include "platform/Win32Util.h"

namespace clipper {

namespace {

// gtk accelerator ("<Primary><Shift>v", what the settings keep on both platforms) -> MOD_* + virtual key
bool parseAccel(const std::string& accel, UINT* mods, UINT* vk) {
    *mods = 0;
    size_t i = 0;
    while (i < accel.size() && accel[i] == '<') {
        const size_t close = accel.find('>', i);
        if (close == std::string::npos) return false;
        const std::string mod = accel.substr(i + 1, close - i - 1);
        if (mod == "Primary" || mod == "Control")
            *mods |= MOD_CONTROL;
        else if (mod == "Shift")
            *mods |= MOD_SHIFT;
        else if (mod == "Alt")
            *mods |= MOD_ALT;
        else if (mod == "Super")
            *mods |= MOD_WIN;
        else
            return false;
        i = close + 1;
    }
    const std::string key = accel.substr(i);
    if (key.size() == 1 && std::isalnum(static_cast<unsigned char>(key[0]))) {
        *vk = static_cast<UINT>(std::toupper(static_cast<unsigned char>(key[0])));  // 'A'..'Z', '0'..'9'
        return true;
    }
    if (key.size() >= 2 && key[0] == 'F' && std::isdigit(static_cast<unsigned char>(key[1]))) {
        const int n = std::atoi(key.c_str() + 1);
        if (n < 1 || n > 24) return false;
        *vk = static_cast<UINT>(VK_F1 + n - 1);
        return true;
    }
    static const struct {
        const char* name;
        UINT vk;
    } kNames[] = {
        {"space", VK_SPACE},         {"comma", VK_OEM_COMMA},       {"period", VK_OEM_PERIOD},
        {"slash", VK_OEM_2},         {"semicolon", VK_OEM_1},       {"apostrophe", VK_OEM_7},
        {"minus", VK_OEM_MINUS},     {"equal", VK_OEM_PLUS},        {"grave", VK_OEM_3},
        {"backslash", VK_OEM_5},     {"bracketleft", VK_OEM_4},     {"bracketright", VK_OEM_6},
        {"Insert", VK_INSERT},       {"Home", VK_HOME},             {"End", VK_END},
        {"Page_Up", VK_PRIOR},       {"Page_Down", VK_NEXT},        {"Pause", VK_PAUSE},
    };
    for (const auto& k : kNames) {
        if (key == k.name) {
            *vk = k.vk;
            return true;
        }
    }
    return false;
}

}  // namespace

ShortcutDesktop shortcutDesktop() { return ShortcutDesktop::Windows; }

const char* shortcutDesktopName(ShortcutDesktop) { return "Windows"; }

bool setGlobalShortcut(const std::string& accel) {
    HWND bus = win32::appBusWindow();
    if (!bus) return false;
    UINT mods = 0, vk = 0;
    if (!accel.empty() && !parseAccel(accel, &mods, &vk)) return false;
    // RegisterHotKey has to run on the thread that owns the window
    return SendMessageW(bus, win32::kMsgSetHotKey, mods, static_cast<LPARAM>(vk)) == 1;
}

// windows doesn't say who has it, RegisterHotKey just fails when it's taken
std::string shortcutConflict(const std::string&) { return {}; }

}  // namespace clipper
