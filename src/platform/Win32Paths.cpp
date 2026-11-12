#include "platform/Win32Util.h"  // windows.h, before the other windows headers

#include <shlobj.h>

#include <filesystem>
#include <system_error>

#include "platform/Paths.h"

namespace clipper {

namespace fs = std::filesystem;

namespace {

// %APPDATA%\Clipper (settings, small, fine to roam) / %LOCALAPPDATA%\Clipper (history + images, can get big)
std::string knownFolder(const KNOWNFOLDERID& id) {
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &path))) return {};
    fs::path dir = fs::path(path) / L"Clipper";
    CoTaskMemFree(path);
    std::error_code ec;
    fs::create_directories(dir, ec);
    return ec ? std::string() : win32::narrow(dir.wstring());
}

}  // namespace

// %CLIPPER_HOME%: settings + history both in there (screenshots, trying things without touching yours)
static std::string homeOverride() {
    const wchar_t* home = _wgetenv(L"CLIPPER_HOME");
    if (!home || !*home) return {};
    std::error_code ec;
    fs::create_directories(home, ec);
    return ec ? std::string() : win32::narrow(home);
}

std::string configDirectory() {
    const std::string home = homeOverride();
    return home.empty() ? knownFolder(FOLDERID_RoamingAppData) : home;
}
std::string dataDirectory() {
    const std::string home = homeOverride();
    return home.empty() ? knownFolder(FOLDERID_LocalAppData) : home;
}

std::string settingsFilePath() {
    const std::string dir = configDirectory();
    return dir.empty() ? std::string() : dir + "\\settings.ini";
}

std::string executablePath() { return win32::narrow(win32::exePath()); }

}  // namespace clipper
