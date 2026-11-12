// dark mode, accent colour and text size from the registry (what Settings > Personalization writes),
// Segoe UI as the font

#include "platform/Win32Util.h"  // windows.h, before the other windows headers

#include <shlobj.h>

#include <algorithm>
#include <filesystem>

#include "platform/Appearance.h"

namespace clipper {

namespace {

bool readDword(const wchar_t* key, const wchar_t* value, DWORD* out) {
    DWORD size = sizeof *out;
    return RegGetValueW(HKEY_CURRENT_USER, key, value, RRF_RT_REG_DWORD, nullptr, out, &size) == ERROR_SUCCESS;
}

std::string fontFile(const wchar_t* name) {
    PWSTR fonts = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Fonts, 0, nullptr, &fonts))) return {};
    const std::filesystem::path file = std::filesystem::path(fonts) / name;
    CoTaskMemFree(fonts);
    std::error_code ec;
    return std::filesystem::exists(file, ec) ? win32::narrow(file.wstring()) : std::string();
}

}  // namespace

SystemAppearance querySystemAppearance() {
    SystemAppearance ap;

    DWORD light = 1;
    readDword(L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", &light);
    ap.dark = light == 0;

    DWORD abgr = 0;  // 0xAABBGGRR
    if (readDword(L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", &abgr)) {
        ap.hasAccent = true;
        ap.accent[0] = static_cast<float>(abgr & 0xFF) / 255.0f;
        ap.accent[1] = static_cast<float>((abgr >> 8) & 0xFF) / 255.0f;
        ap.accent[2] = static_cast<float>((abgr >> 16) & 0xFF) / 255.0f;
    }

    // Segoe UI Variable (win 11) is a variable font, imgui's rasterizer only does the static one
    ap.fontFile = fontFile(L"segoeui.ttf");
    DWORD textScale = 100;  // Settings > Accessibility > Text size, 100..225
    readDword(L"Software\\Microsoft\\Accessibility", L"TextScaleFactor", &textScale);
    ap.fontSizePx = 14.0f * static_cast<float>(std::clamp<DWORD>(textScale, 100, 225)) / 100.0f;  // win 11 body text
    return ap;
}

}  // namespace clipper
