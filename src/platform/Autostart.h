#pragma once

namespace clipper {

// ~/.config/autostart/clipper.desktop, launched with --minimized.
// shows up in mint's "Startup Applications" too
bool isLaunchAtLoginEnabled();
bool setLaunchAtLogin(bool enable);

}  // namespace clipper
