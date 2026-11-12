| clipboard  | `Clipboard` (X11Clipboard.cpp): # Architecture

```
src/
  core/       clips, the history journal, search, file uris, png size. plain c++, no os calls
  platform/   one header per job (Clipboard, Desktop, Tray, AppBus, GlobalShortcut, Paths, Autostart,
              Appearance, Shell), implemented twice: X11* / plain names for linux, Win32* for windows
  app/        imgui gui: popup, manager, preferences, thumbnails. shared, a few #ifdef _WIN32 for wording
data/windows/ clipper.rc.in (icon + version info), clipper.manifest (utf-8 code page, per-monitor dpi)
installer/    Clipper.iss (Inno Setup), Clipper.wxs (WiX)
tests/        core tests
```

The rest of this file is about Linux. Windows is at the end.

## Threads

| thread     | does                                                                                |
|------------|-------------------------------------------------------------------------------------|
| ui         | owns the `ClipStore`, draws, uses glfw's X connection for focus / pointer / XTest   |
| clipboard  | `X11Clipboard`: its own X connection, watches + reads CLIPBOARD, serves our clips    |
| thumbnails | decodes + shrinks pngs for the gl textures                                          |
| tray, bus  | sd-bus: StatusNotifierItem + dbusmenu, and `io.github.KimsFerdy.Clipper` (single instance) |

Each X connection only gets used from one thread. `XInitThreads()` runs before anything else anyway.

## Capturing

XFixes tells us when the CLIPBOARD owner changes, so there's no polling. On each change: ask for `TARGETS`,
pick one, and read it (INCR for big data):

1. `x-special/gnome-copied-files`: files copied in nemo / nautilus / caja, saved as `copy`/`cut` + uris
2. `UTF8_STRING`, `text/plain;charset=utf-8`, `text/plain`, `STRING`: text (beats images, office suites
   offer both and mean the text)
3. `image/png`
4. `text/uri-list` if every line is a local `file://` uri

### Copies that aren't new

When an app quits, Muffin / Mutter (and csd-clipboard) take its clipboard over and serve a copy of the text,
without markers like `x-kde-passwordManagerHint`. That shows up as an owner change like any other. So:

- content equal to the last capture (or the text form of the last copied files) isn't a new copy
- secrets (targets include the password hint) get read but only their hash is kept in memory. The same
  content showing up again later is skipped too
- if nobody takes over within 300 ms, we put the last capture back ourselves

## Pasting from the popup

1. the shortcut runs `Clipper --popup`. The new process hands the request to the running one over d-bus and exits
2. we remember `_NET_ACTIVE_WINDOW` (the paste target) and the pointer, then show the popup there. It's the
   same glfw window as the manager, switched to undecorated + utility type + skip taskbar while unmapped
3. on `Enter` the clipboard thread takes CLIPBOARD (and PRIMARY for terminals) with the clip and waits for the
   server to confirm, then we hide, ask the wm to activate the target, and wait until it's active
4. XTest (libXtst.so.6, dlopen'ed): release held modifiers, then `Ctrl+V`, or `Shift+Insert` for terminals
   (their Ctrl+Shift+V is our shortcut now). No paste if the target never got focus back

We serve `TARGETS`, `TIMESTAMP` and per kind: text as `UTF8_STRING` / `text/plain*` / `STRING`; images as
`image/png` (INCR above 256 KB); files as `x-special/gnome-copied-files` (always `copy`, a replayed cut would
find nothing to move), `text/uri-list` and the paths as text.

## History file

`~/.local/share/clipper/history.bin` is an append-only journal:

```
header  "CLIPHIST" u32 version u32 0
entry   u32 size, u32 crc32, body (op + fields)
ops     PutClip (whole clip), Meta (pin / folder / title / order), Remove, PutFolder, RemoveFolder
```

Every change appends one entry and flushes it. Startup replays everything. A bad crc or a short entry at
the end (crash mid-write) ends the replay, and the file gets rewritten. When more than half of a >256 KB
file is dead, `compact()` writes only what's alive to a tmp file and renames it over. A file that isn't
ours gets moved aside to `history.bin.unreadable`, never overwritten.

Images aren't in the journal: `images/<content hash>.png`, removed together with their clip. The same
content copied again (same hash) moves the existing clip to the top instead of adding a new one.

Pinned clips and clips in folders are "kept": never pruned by the history limit, never cleared.

## Windows

Same app, same core, same imgui code. The platform layer is Win32 only, no extra libraries:

| piece          | how                                                                                          |
|----------------|----------------------------------------------------------------------------------------------|
| clipboard      | message-only window on its own thread, `AddClipboardFormatListener`. After a change it waits 60 ms (apps add formats one by one, and keep the clipboard open a moment), then copies out one of: `CF_HDROP` (+ `Preferred DropEffect` for cut), `CF_UNICODETEXT`, `PNG`, `CF_DIBV5` / `CF_DIB` (encoded to png with stb_image_write). Same order as on linux |
| secrets        | skipped when `ExcludeClipboardContentFromMonitorProcessing`, `Clipboard Viewer Ignore` or `CanIncludeInClipboardHistory = 0` are there |
| our own copies | everything goes on the clipboard right away (no delayed rendering), so it outlives us. Its sequence number is remembered so the listener skips it |
| single instance | named mutex `Local\io.github.KimsFerdy.Clipper` + hidden window `ClipperAppBus`. A second launch sends its request as `WM_COPYDATA` and exits |
| shortcut       | `RegisterHotKey` on that same window, set again on every start. `WM_HOTKEY` = popup, no second process |
| closing        | that window turns `WM_ENDSESSION` (log off, Restart Manager) and `WM_CLOSE` into Quit. Installers run `Clipper --quit` |
| paste          | `SetForegroundWindow` (with `AttachThreadInput` if windows says no), wait until it's active, `SendInput` Ctrl+V. Held Alt / Win get a Ctrl tap before they're released, so no menu bar / start menu |
| tray           | `Shell_NotifyIcon` from a hidden top-level window (message-only ones miss `TaskbarCreated`), icon `GLFW_ICON` from the exe |
| paths          | `%LOCALAPPDATA%\Clipper` (history, images), `%APPDATA%\Clipper\settings.ini`. All strings stay utf-8: the manifest makes utf-8 the process code page, so the core's `fopen` / `std::filesystem` work with any user name |
| file uris      | `C:\a b` ↔ `file:///C:/a%20b`, `\nas\share` ↔ `file://nas/share` (FileList.cpp, `#ifdef _WIN32`) |

The exe links the CRT statically and only needs system DLLs.
