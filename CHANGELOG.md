# Changelog

## 0.1.0-beta (2026-10-18)

First version, early beta.

### Capture
- watches the clipboard with XFixes (no polling): text, images (png, up to 64 MB) and files copied in the
  file manager
- the same content copied again moves the existing clip to the top
- skips copies marked as secret by password managers, also when the desktop re-serves them after the app quits
- never saves copies from excluded apps (KeePassXC, Bitwarden, 1Password by default)
- pause / resume from the tray or the menu
- keeps the last copy pasteable when the app it came from quits

### Popup (Ctrl+Shift+V)
- opens at the mouse over any app, search as you type
- Enter pastes into the app you were in (Ctrl+V, Shift+Insert in terminals), Ctrl+Enter only copies
- Ctrl+1..9, Tab for pinned clips, Ctrl+P pin, Shift+Del delete, preview of the selected clip
- copied files paste as a copy into whatever folder the file manager shows

### Manager
- history / pinned / folders sidebar, filter by text / images / files, search
- titles, edit text clips, pin, move to folders (menu or drag and drop), delete, clear history
- image previews, file lists with missing files marked

### Desktop
- tray icon with Open Clipper / Pause capturing / Quit
- starts at login (tray only), single instance over D-Bus
- Cinnamon / GNOME custom shortcut set up on the first run
- system font, dark / light, accent colour; zoom
