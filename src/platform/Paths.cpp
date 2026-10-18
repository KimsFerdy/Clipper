#include "platform/Paths.h"

#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace clipper {

namespace fs = std::filesystem;

namespace {

// $XDG_<KIND>_HOME if set, ~/<fallback> otherwise
fs::path xdgDir(const char* var, const char* fallback) {
    const char* xdg = std::getenv(var);
    if (xdg && *xdg) return fs::path(xdg) / "clipper";
    const char* home = std::getenv("HOME");
    return home ? fs::path(home) / fallback / "clipper" : fs::path();
}

std::string ensureDir(const fs::path& dir) {
    if (dir.empty()) return {};
    std::error_code ec;
    fs::create_directories(dir, ec);
    return ec ? std::string() : dir.string();
}

std::string fileIn(const std::string& dir, const char* name) {
    return dir.empty() ? std::string() : (fs::path(dir) / name).string();
}

}  // namespace

std::string configDirectory() { return ensureDir(xdgDir("XDG_CONFIG_HOME", ".config")); }
std::string dataDirectory() { return ensureDir(xdgDir("XDG_DATA_HOME", ".local/share")); }
std::string settingsFilePath() { return fileIn(configDirectory(), "settings.ini"); }

std::string executablePath() {
    // inside an AppImage /proc/self/exe points into a temporary mount, $APPIMAGE is the real file
    if (const char* appimage = std::getenv("APPIMAGE"); appimage && *appimage) return appimage;
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? std::string() : p.string();
}

}  // namespace clipper
