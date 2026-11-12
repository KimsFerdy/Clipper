# Changelog

## 1.0.0 (2026-11-12)

Clipper runs on Windows now, everything from the Linux beta works there too.

### Windows (new)
- clipboard watched with AddClipboardFormatListener (no polling): text, images (PNG, or bitmaps turned into
  PNG) and files copied in Explorer, cut included
- skips copies marked secret (ExcludeClipboardContentFromMonitorProcessing, Clipboard Viewer Ignore,
  CanIncludeInClipboardHistory = 0), KeePass added to the excluded apps
- `Ctrl+Shift+V` popup over any app with a global hotkey, Enter pastes with Ctrl+V (Shift+Insert in mintty /
  PuTTY / Alacritty / WezTerm)
- pasting files from the popup into an Explorer folder copies them there
- tray icon (click opens, right click for the menu), starts at login (Task Manager > Startup apps),
  single instance
- follows dark / light mode, the accent colour and the text size setting, dark title bar
- packages: setup.exe (Inno Setup), .msi (WiX), portable .zip. Per user, no admin, no runtime to install

### New look (both)
- calmer, minimal theme: neutral greys, the list and the details as rounded cards, selection as a soft raised
  row instead of an accent block, outline icons, counts in little badges, a toggle for Pinned
- popup: search field with a magnifier, History / Pinned as a segmented switch
- the accent colour only for small things now (checkmarks, pinned bookmark, text selection)

### Both
- `CLIPPER_HOME=<dir>` keeps settings + history in another folder, `--screenshot FILE [--popup]` renders a view
  with sample clips into a png (the README pictures)
- `--quit` closes the running Clipper (installers use it to update)
- "Open containing folder" selects the file on Windows


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
