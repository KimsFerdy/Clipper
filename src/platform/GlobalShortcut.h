#pragma once

#include <string>

namespace clipper {

// linux: global shortcut = a custom keyboard shortcut in the desktop's own settings that runs
// `Clipper --popup`. no key grabbing of our own, so it shows up (and can be edited) in
// the desktop's keyboard settings and keeps working on wayland.
// windows: RegisterHotKey on the AppBus window, for as long as we run. set again on every start
enum class ShortcutDesktop { Cinnamon, Gnome, Windows, Unsupported };

ShortcutDesktop shortcutDesktop();
const char* shortcutDesktopName(ShortcutDesktop desktop);

// accel in gtk format ("<Super>f"), "" removes our shortcut. false if gsettings refused / the key is taken.
// the command is this binary with --popup
bool setGlobalShortcut(const std::string& accel);

// name of an existing desktop binding that already uses accel, or ""
std::string shortcutConflict(const std::string& accel);

// "<Super><Shift>f" -> "Super+Shift+F"
std::string prettyAccel(const std::string& accel);

}  // namespace clipper
