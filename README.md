<picture>
  <source media="(prefers-color-scheme: dark)" srcset="brand/dockx-logo.png">
  <source media="(prefers-color-scheme: light)" srcset="brand/dockx-logo-light.png">
  <img src="brand/dockx-logo.png" alt="DockX" width="280">
</picture>

**Flexible dock layouts for OBS Studio. Your OBS, your way.**

DockX is a free, native OBS Studio plugin that unlocks the dock layouts OBS never let you
build, then adds on the layout tools OBS always needed. It lives inside OBS under
**Tools > DockX**.

> **[Download DockX](https://strmrx.com/dockx)** &nbsp;·&nbsp; [Guides](https://strmrx.com/dockx) &nbsp;·&nbsp; [Support on Ko-fi](https://ko-fi.com/strmrx) &nbsp;·&nbsp; by [StrmrX](https://strmrx.com)

Free, with every feature unlocked, and it stays that way.

---

## Why DockX

Because streamers and broadcasters deserve a better experience.

From a dock system that is finally unlocked and fully customizable, to search tools, folder
organization, and pinning third party apps directly into your OBS setup, DockX is such a
glow up from base OBS that you will wonder how you ever streamed without it.

And it is built by a streamer who uses OBS daily, inspired by the details and pain points he
runs into every single day. This is truly a plugin made to solve the everyday problems of
streamers.

## Features

- **Flexible dock nesting** - full-height columns beside stacked panels; build the layout OBS never allowed.
- **Wide docks** - stretch a dock across a whole row, or across docks of your choosing.
- **Scene Folders** - a collapsible, searchable folder tree over your scene list, with live thumbnails and a grid view.
- **Pin any window** - dock an outside app (a TikTok Live Studio chat, a browser, a music player) into your OBS layout (Windows).
- **Colors and themes** - recolor OBS to your liking. Customize it yourself and save the look, or use our premade one-click looks.
- **DockX Preview** - a live, editable canvas inside a dock. Move, resize, rotate and crop sources from a panel, with snapping and studio-mode support. Per-dock zoom and pan. Replaces the base OBS scene preview, which is VERY limited in orientation, and lets you scale and move the preview however you like.
- **Saved layouts and hotkeys** - snapshot any dock arrangement and bring it back with a hotkey, or auto-switch it with the scene. Lock a layout so it never drifts mid stream.
- **Source loadouts** - save where every source sits and snap them all back in one click.
- **Find any source** - one search box scans every scene at once; jump to a source, open its properties, clean up orphans.
- **Source tags + bulk actions** - label sources, then Show / Hide / Lock / Mute a whole group across every scene at once.
- **Second-screen container** - gather panels into one managed, nestable window on another monitor.
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
