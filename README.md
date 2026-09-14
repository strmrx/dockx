<picture>
  <source media="(prefers-color-scheme: dark)" srcset="brand/dockx-logo.png">
  <source media="(prefers-color-scheme: light)" srcset="brand/dockx-logo-light.png">
  <img src="brand/dockx-logo.png" alt="DockX" width="280">
</picture>

**Flexible dock layouts for OBS Studio. Your OBS, your way.**

DockX is a free, native OBS Studio plugin that unlocks the dock layouts OBS never let you
build, then piles on the layout tools OBS always needed. It lives inside OBS under
**Tools > DockX**.

> **[Download DockX](https://strmrx.com/dockx)** &nbsp;·&nbsp; [Guides](https://strmrx.com/dockx) &nbsp;·&nbsp; [Support on Ko-fi](https://ko-fi.com/strmrx) &nbsp;·&nbsp; by [StrmrX](https://strmrx.com)

Free, with every feature unlocked, and it stays that way.

---

## Why DockX

OBS's dock system is row based: you cannot put a full-height chat column next to a second
column of stacked panels. The window framework OBS is built on has supported nested docks
for years. OBS just never turned it on. DockX turns it on from inside OBS, and makes the
whole thing a product.

## Features

- **Flexible dock nesting** - full-height columns beside stacked panels; build the layout OBS never allowed.
- **DockX Preview** - a live, editable canvas inside a dock. Move, resize, rotate and crop sources from a panel, with snapping and studio-mode support. Per-dock zoom and pan.
- **Scene Folders** - a collapsible, searchable folder tree over your scene list, with live thumbnails and a grid view.
- **Saved layouts and hotkeys** - snapshot any dock arrangement and bring it back with a hotkey, or auto-switch it with the scene. Lock a layout so it never drifts mid stream.
- **Source loadouts** - save where every source sits and snap them all back in one click.
- **Find any source** - one search box scans every scene at once; jump to a source, open its properties, clean up orphans.
- **Source tags + bulk actions** - label sources, then Show / Hide / Lock / Mute a whole group across every scene at once.
- **Wide docks** - stretch a dock across a whole row.
- **Pin any window** - dock an outside app (a TikTok Live Studio chat, a browser, a music player) into your OBS layout (Windows).
- **Second-screen container** - gather panels into one managed, nestable window on another monitor.
- **Colors and themes** - recolor OBS to your brand, with one-click looks.
- **Align and distribute** - line up, space evenly, and center on canvas from a proper Align tab.

## Install (Windows)

DockX is a Windows plugin for now. Two ways to install:

1. **Installer (recommended):** download the `.exe` from [strmrx.com/dockx](https://strmrx.com/dockx)
   or the [Releases page](https://github.com/strmrx/dockx/releases). Close OBS, run it, done.
   It finds OBS for you and adds an uninstaller.
2. **Zip (manual):** download the `.zip`, close OBS, and run `install.bat` as administrator
   (or copy `dockx.dll` into `obs-plugins\64bit\` and `locale\en-US.ini` into
   `data\obs-plugins\dockx\locale\` inside your OBS folder).

Then open OBS and look under **Tools > DockX**.

### About the Windows security warning

Windows may show a blue "Windows protected your PC" box, and antivirus may pause to scan the
files. This is normal for new software from a small publisher without an expensive code
signing certificate. It does not mean anything is wrong. DockX is open source (you are
reading its repo), and the installer only copies the plugin into your OBS folder. Click
"More info" then "Run anyway" to continue.

## Support development

DockX is free and every feature is unlocked. If it earns a spot in your setup, a tip keeps
the updates coming: **[Support on Ko-fi](https://ko-fi.com/strmrx)**. No pressure, and
nothing is ever locked behind it.

## Build from source

DockX is based on the official [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate).
Dependencies (libobs, obs-frontend-api, Qt6) are downloaded at configure time.

```sh
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo
```

Output is `dockx.dll`. See `CLAUDE.md` and `docs/concept.md` for architecture and roadmap.

## License

GPL v2, inherited from OBS Studio and the plugin template. See [LICENSE](LICENSE).

---

Made by [StrmrX](https://strmrx.com) - tools built by streamers, for streamers.
