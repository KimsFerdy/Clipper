#pragma once

#include <string>

namespace clipper {

// xdg dirs, created if missing. "" if HOME isn't set or mkdir fails
std::string configDirectory();  // $XDG_CONFIG_HOME/clipper (~/.config/clipper)
std::string dataDirectory();    // $XDG_DATA_HOME/clipper   (~/.local/share/clipper), the history lives here

std::string settingsFilePath();  // <config>/settings.ini

// $APPIMAGE if we run from one, /proc/self/exe otherwise
std::string executablePath();

}  // namespace clipper
