# Architecture

```
src/
  core/       clips, the history journal, search, file uris, png size. plain c++, no os calls
  platform/   x11 clipboard + desktop, tray, d-bus single instance, global shortcut, xdg, autostart
  app/        imgui gui: popup, manager, preferences, thumbnails
tests/        core tests
```

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
