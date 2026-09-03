# Uninstall / revert DockX

DockX is built to be **fully reversible**. It never edits OBS's core install, never
touches your source media (video files, images, captures), and everything it does write
can be undone. This doc is the canonical reference; the same guidance lives in
user-friendly form on the strmrx.com/dockx help page.

## What DockX writes (the whole footprint)

Just three things, all reversible:

1. **The plugin itself** -- two files placed by the installer:
   - `C:\Program Files\obs-studio\obs-plugins\64bit\dockx.dll`
   - `C:\Program Files\obs-studio\data\obs-plugins\dockx\locale\en-US.ini`
2. **One settings file** -- `%APPDATA%\obs-studio\plugin_config\dockx.json`
   (your layouts, hotkeys, folders, colors, and toggles). Written atomically with a
   temp-then-rename plus a backup, so a crash mid-save can't corrupt it.
3. **Your dock arrangement** -- saved and restored through OBS's own dock store
   (Qt `saveState` / `restoreState`). This is the same store OBS uses for its own docks;
   DockX does not keep a private copy of your window layout.

That's it. No registry keys, no files outside those locations, no changes to OBS's
program files beyond the two plugin files above.

## In-app escape hatches (no uninstall needed)

If a layout ever looks wrong, you usually don't need to remove anything:

- **Tools > DockX: Revert dock layout** -- snaps the docks back to your saved revert point.
- **Tools > DockX: Rescue docks to this screen** -- pulls any floating or off-screen dock
  back onto your current monitor (for when a dock ends up on a display you've unplugged).
- **Tools > DockX: Show or hide OBS preview** -- brings OBS's built-in center preview back
  if you had collapsed it. The way back to the stock preview can never be lost.

## If a plugin ever stopped OBS from starting

It shouldn't -- DockX is defensive and does nothing when anything looks off -- but if any
third-party plugin ever prevents OBS from launching, OBS offers **Safe Mode** automatically
after a crash. Safe Mode boots with all third-party plugins (including DockX) disabled, so
you are never locked out of OBS. From there you can remove the plugin normally.

## Full clean uninstall

Close OBS first, then either:

**Option A -- run the uninstaller** (easiest):
Right-click `dist\uninstall.bat` and pick **Run as administrator**. It removes the DLL and
the locale folder. It intentionally leaves `dockx.json` in place so your layouts survive a
reinstall; delete it yourself if you want a truly blank slate (see Option B step 3).

**Option B -- by hand** (delete these three, in any order):
1. `C:\Program Files\obs-studio\obs-plugins\64bit\dockx.dll`
2. `C:\Program Files\obs-studio\data\obs-plugins\dockx\` (the whole DockX folder, which
   holds the locale file)
3. `%APPDATA%\obs-studio\plugin_config\dockx.json` -- optional. Delete it only if you also
   want to discard your saved layouts, hotkeys, and settings. Keeping it means a future
   reinstall picks up exactly where you left off.

Restart OBS. Your scenes, sources, and media are untouched -- DockX only ever arranged and
edited them, the same way OBS's own tools do.

## The one real edit surface

The only place DockX changes your actual OBS project (not just window layout) is the
**DockX Preview** editor, which moves, resizes, rotates, and crops scene items. Those are
ordinary scene-item transforms -- identical to editing in OBS's built-in preview, saved in
your scene collection, and reversible with Ctrl+Z or by re-editing. DockX never rewrites or
deletes the underlying source files.
