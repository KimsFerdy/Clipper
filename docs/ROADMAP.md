# Roadmap

## 0.1
- [x] X11 clipboard watcher (XFixes) + owner (INCR both ways)
- [x] text, png images, copied files
- [x] append-only history journal, compaction, crash-safe tail
- [x] popup at the mouse, auto-paste via XTest, terminals via Shift+Insert
- [x] manager: pins, folders, titles, edit, drag and drop, type filter
- [x] password manager hint, excluded apps, pause
- [x] tray, autostart, single instance, global shortcut (Cinnamon / GNOME)

## Next
- [ ] multi-select in the manager (delete / move many)
- [ ] paste as plain text (strip formatting) once rich text (text/html) is captured
- [ ] "Paste into folder..." for files clips right from the manager (finish the copy without a file manager)
- [ ] save an image clip as a file, drag clips out to other apps (xdnd)
- [ ] snippets: clips with placeholders like {date}
- [ ] import / export folders
- [ ] Wayland (wlr-data-control / ext-data-control)
- [ ] .deb / AppImage packaging like FindEverything
