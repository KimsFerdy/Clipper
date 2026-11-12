<p align="center">
  <img src="assets/clipper.png" width="112" alt="Clipper icon">
</p>

<h1 align="center">Clipper</h1>

<p align="center">
  <b>Everything you copy, one shortcut away.</b><br>
  Clipboard history for Windows and Linux, inspired by <a href="https://clipclip.com/">ClipClip</a>.
</p>

<p align="center">
  <a href="https://github.com/KimsFerdy/Clipper/releases/latest"><img src="https://img.shields.io/github/v/release/KimsFerdy/Clipper?include_prereleases&label=download&style=for-the-badge&color=1f9ede" alt="Download"></a>
  <img src="https://img.shields.io/badge/platform-windows%20%7C%20linux%20(x11)-1f9ede?style=for-the-badge" alt="Windows and Linux X11">
  <a href="LICENSE"><img src="https://img.shields.io/github/license/KimsFerdy/Clipper?style=for-the-badge&color=1f9ede" alt="MIT license"></a>
</p>

<p align="center">
  <img src="docs/screenshots/popup-dark.png" alt="The Ctrl+Shift+V popup" width="520">
</p>

---

Clipper remembers what you copy: text, images and files. `Ctrl+V` pastes as usual. Press `Ctrl+Shift+V`
and a popup opens at your mouse with your history. Type to filter, press `Enter`, and the clip gets pasted
into the app you were in.

- **Popup at the mouse.** Search as you type, `Enter` pastes, `Ctrl+1`…`9` pastes that row
- **Text, images and files.** Screenshots and copied pictures are kept as PNG. Files copied in the file
  manager are kept as a copy command: paste them from the popup into any folder and they get copied there
- **Organize.** Pin clips, give them titles, edit them, and drag them into folders. Pinned clips and clips in
  folders are never cleaned up
- **Keeps the clipboard alive.** When the app you copied from quits, what you copied stays pasteable
- **Leaves passwords alone.** Copies that password managers mark as secret (KeePassXC, KeePass) are never saved.
  You can also exclude apps by name, or pause capturing from the tray
- **Feels native.** Uses your system font, dark / light mode and accent colour, and lives in the tray
- **Small.** One binary, C++20 + Dear ImGui, no Qt, GTK or .NET. Under 1.5 MB on Windows

## Download

<p>
  <a href="https://github.com/KimsFerdy/Clipper/releases/latest"><b>➜ Get the latest release</b></a>
</p>

### Windows 10 / 11

| Package | Best for | Install |
|---|---|---|
| `Clipper-*-windows-x64-setup.exe` | Most people | Run it. Installs for you only, no admin needed |
| `Clipper-*-windows-x64.msi` | IT / scripted installs | `msiexec /i Clipper-1.0.0-windows-x64.msi /qn` (per user) |
| `Clipper-*-windows-x64.zip` | Portable, no install | Unzip anywhere and run `Clipper.exe` |

The binaries aren't code-signed yet, so SmartScreen may say *Windows protected your PC*: click *More info* →
*Run anyway*.

### Linux

| Package | Best for | Install |
|---|---|---|
| `.deb` | Ubuntu 24.04+, Linux Mint 22+, Debian 13+ | `sudo apt install ./clipper_1.0.0_amd64.deb` |
| `AppImage` | Any recent distro, no install | `chmod +x Clipper-*.AppImage && ./Clipper-*.AppImage` |
| `.tar.gz` | Manual install into `~/.local` | `tar xzf clipper-*.tar.gz && cp -r clipper-*/{bin,share} ~/.local/` |

Linux binaries are x86_64, built on Linux Mint 22 (glibc 2.39). Checksums are in `SHA256SUMS` on the release page.

## Screenshots

<p align="center">
  <img src="docs/screenshots/manager-dark.png" alt="The manager in dark mode" width="860">
</p>
<p align="center">
  <img src="docs/screenshots/manager-light.png" alt="The manager in light mode" width="860">
</p>

## Keys

| Popup                 |                                   |
|-----------------------|-----------------------------------|
| `Ctrl+Shift+V`        | open it (from any app)            |
| type                  | search                            |
| `↑` `↓`               | pick a clip                       |
| `Enter` / click       | paste it into the app you were in |
| `Ctrl+Enter`          | put it on the clipboard only      |
| `Ctrl+1` … `Ctrl+9`   | paste that row                    |
| `Tab`                 | history / pinned                  |
| `Ctrl+P`              | pin / unpin                       |
| `Shift+Del`           | delete                            |
| `Ctrl+M`              | open the manager                  |
| `Esc`                 | close                             |

In the manager: `Enter` or a double click copies, `Ctrl+F` searches, `F2` sets a title, `Del` deletes,
and you can drag clips onto *Pinned* or a folder.

### Terminals

Clipper owns `Ctrl+Shift+V` now, so apps that use it themselves (Linux terminals, "paste as plain text" in
some Windows apps) don't get it any more. Pasting through the popup still works everywhere: Clipper
recognises terminals that want it and pastes with `Shift+Insert` there (on Windows: mintty / Git Bash, PuTTY,
Alacritty, WezTerm. Windows Terminal takes `Ctrl+V`). If you'd rather keep `Ctrl+Shift+V`, pick another
shortcut in *Preferences*.

## Requirements

**Windows**
- Windows 10 or 11, x64
- Auto-paste can't reach apps running as administrator (Windows doesn't let a normal app type into them).
  The clip is still on the clipboard, paste it with `Ctrl+V`

**Linux**
- An **X11** session (Cinnamon, MATE, Xfce, GNOME on Xorg...). Wayland isn't supported yet
- The shortcut gets set up automatically on Cinnamon and GNOME. On other desktops, add a custom shortcut that
  runs `Clipper --popup`
- Auto-paste needs `libxtst6`, which comes with every desktop install

## Build from source

**Linux**

```bash
scripts/install-deps-linux.sh        # debian / ubuntu / mint
cmake --preset release
cmake --build --preset release
./build/release/Clipper
cmake --install build/release --prefix ~/.local   # app menu entry + icon
scripts/package.sh                   # .deb, AppImage, .tar.gz into dist/
```

**Windows** (Visual Studio 2022 with the C++ workload; it brings cmake and ninja)

```powershell
.\scripts\package-windows.ps1        # release build + tests + zip, setup.exe and .msi into dist\
```

The script sets up the MSVC environment by itself. The installers need
[Inno Setup 6](https://jrsoftware.org/isinfo.php) (`winget install JRSoftware.InnoSetup`) and WiX 5
(`dotnet tool install --global wix --version 5.0.2`, then
`wix extension add -g WixToolset.UI.wixext/5.0.2 WixToolset.Util.wixext/5.0.2`). For day to day work, run
`cmake --preset debug` + `cmake --build --preset debug` from a *Developer PowerShell for VS 2022*.

The first configure downloads GLFW, Dear ImGui and stb (git + network needed once).
Run the tests with `ctest --preset debug` after `cmake --preset debug && cmake --build --preset debug`.

## Where things are

| What           | Linux                                            | Windows                                       |
|----------------|--------------------------------------------------|-----------------------------------------------|
| History        | `~/.local/share/clipper/history.bin`             | `%LOCALAPPDATA%\Clipper\history.bin`          |
| Images         | `~/.local/share/clipper/images/*.png`            | `%LOCALAPPDATA%\Clipper\images\*.png`         |
| Settings       | `~/.config/clipper/settings.ini` (hand-editable) | `%APPDATA%\Clipper\settings.ini`              |
| Start at login | `~/.config/autostart/clipper.desktop`            | *Clipper* in Task Manager › Startup apps      |
| Installed to   | `/usr/bin` (.deb), `~/.local/bin` (.tar.gz)      | `%LOCALAPPDATA%\Programs\Clipper`             |

## Command line

```
Clipper              open the manager (or start in the tray)
Clipper --popup      open the history popup at the mouse (what the shortcut runs on Linux)
Clipper --toggle     show / hide the manager
Clipper --minimized  start in the tray (the login autostart uses this)
Clipper --quit       close the running Clipper
```

For screenshots and trying things out: `CLIPPER_HOME=<dir>` keeps settings and history in that folder instead of
yours, and `Clipper --screenshot out.png [--popup]` draws the manager (or the popup) into a png without opening a
window, with sample clips when the history is empty. That's how the pictures above are made.

## License

MIT
